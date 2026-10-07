/* Surfer DOM prelude: the parts of the web platform written in JavaScript,
 * on top of the native primitives in js.c (names starting with "__"). */
(function () {
'use strict';
const G = globalThis;
const doc = G.document;
const t0 = __now();
const XHTML_NS = 'http://www.w3.org/1999/xhtml';
const SVG_NS = 'http://www.w3.org/2000/svg';

function def(obj, name, get, set) {
    Object.defineProperty(obj, name, { get, set, configurable: true, enumerable: true });
}
function val(obj, name, value) {
    Object.defineProperty(obj, name, { value, writable: true, configurable: true, enumerable: false });
}

/* An array that contains itself joins as '' where it repeats, as in
 * browsers; QuickJS recursed until the stack ran out (YouTube's player
 * stringifies such arrays). */
{
    const AP = Array.prototype, joining = new Set();
    for (const name of ['join', 'toLocaleString']) {
        const native = AP[name];
        val(AP, name, {
            [name](...args) {
                if (joining.has(this)) return '';
                joining.add(this);
                try { return native.apply(this, args); } finally { joining.delete(this); }
            }
        }[name]);
    }
}

G.window = G; G.self = G; G.top = G; G.parent = G; G.frames = G;
G.frameElement = null;
G.opener = null;
G.closed = false;
G.name = '';
G.length = 0;

/* ---- console ------------------------------------------------------------ */
function show(v, depth) {
    if (typeof v === 'string') return v;
    if (v instanceof Error) return (v.name || 'Error') + ': ' + v.message + (v.stack ? '\n' + v.stack : '');
    if (v && typeof v === 'object' && typeof v.nodeType === 'number') return '<' + (v.nodeName || '?').toLowerCase() + '>';
    if (typeof v === 'function') return 'function ' + (v.name || '');
    if (typeof v === 'symbol') return v.toString();
    try {
        const s = JSON.stringify(v);
        return s === undefined ? String(v) : (s.length > 400 ? s.slice(0, 400) + '…' : s);
    } catch (e) { return String(v); }
}
const fmt = (args) => Array.prototype.map.call(args, (a) => show(a)).join(' ');
const counts = {}, timers = {};
G.console = {
    log() { __log(fmt(arguments)); }, info() { __log(fmt(arguments)); }, debug() { __log(fmt(arguments)); },
    warn() { __log('warn: ' + fmt(arguments)); },
    error() {
        __log('error: ' + fmt(arguments));
        /* like a browser console: where an Error came from (more of it for
         * stack overflows, so the repeating cycle shows) */
        for (const a of arguments) if (a instanceof Error && a.stack) {
            const frames = String(a.stack).split('\n').map((s) => s.trim()).filter(Boolean);
            const n = /stack overflow/.test(a.message) ? 90 : 6;
            __log('    ' + frames.slice(0, n).join(' | '));
        }
    },
    trace() { __log('trace: ' + fmt(arguments)); }, dir(o) { __log(show(o)); }, dirxml(o) { __log(show(o)); },
    table(o) { __log(show(o)); }, group() {}, groupCollapsed() {}, groupEnd() {},
    time(l) { timers[l || 'default'] = __now(); },
    timeEnd(l) { l = l || 'default'; __log(l + ': ' + (__now() - (timers[l] || 0)).toFixed(1) + 'ms'); },
    timeLog() {}, count(l) { l = l || 'default'; counts[l] = (counts[l] || 0) + 1; __log(l + ': ' + counts[l]); },
    countReset(l) { counts[l || 'default'] = 0; },
    assert(c) { if (!c) __log('assertion failed: ' + fmt(Array.prototype.slice.call(arguments, 1))); },
    clear() {}
};
function reportError(e) {
    try { console.error(e); } catch (x) {}
    try {
        if (typeof G.onerror === 'function') G.onerror(String(e && e.message || e), '', 0, 0, e);
    } catch (x) {}
}
G.reportError = reportError;

/* ---- timers ------------------------------------------------------------- */
const asFn = (fn) => typeof fn === 'function' ? fn : () => (0, eval)(String(fn));
G.setTimeout = (fn, delay, ...args) => __setTimer(asFn(fn), +delay || 0, false, false, ...args);
G.setInterval = (fn, delay, ...args) => __setTimer(asFn(fn), +delay || 0, true, false, ...args);
G.clearTimeout = G.clearInterval = (id) => { if (id) __clearTimer(id | 0); };
G.requestAnimationFrame = (fn) => __setTimer(fn, 16, false, true);
G.cancelAnimationFrame = (id) => { if (id) __clearTimer(id | 0); };
G.queueMicrotask = (fn) => { Promise.resolve().then(fn).catch(reportError); };
G.requestIdleCallback = (fn) => setTimeout(() => fn({ didTimeout: false, timeRemaining: () => 12 }), 1);
G.cancelIdleCallback = G.clearTimeout;
G.setImmediate = (fn, ...args) => setTimeout(fn, 0, ...args);
G.clearImmediate = G.clearTimeout;

G.performance = {
    timeOrigin: Date.now(),
    now: () => __now() - t0,
    mark() {}, measure() {}, clearMarks() {}, clearMeasures() {},
    getEntries: () => [], getEntriesByName: () => [], getEntriesByType: () => [],
    timing: { navigationStart: Date.now() }, navigation: { type: 0 }
};

/* ---- events ------------------------------------------------------------- */
class Event {
    constructor(type, init) {
        init = init || {};
        this.type = String(type);
        this.bubbles = !!init.bubbles;
        this.cancelable = !!init.cancelable;
        this.composed = !!init.composed;
        this.defaultPrevented = false;
        this.target = null;
        this.currentTarget = null;
        this.eventPhase = 0;
        this.isTrusted = false;
        this.timeStamp = performance.now();
        this._stop = false;
        this._stopNow = false;
        this._path = [];
    }
    preventDefault() { if (this.cancelable) this.defaultPrevented = true; }
    stopPropagation() { this._stop = true; }
    stopImmediatePropagation() { this._stop = true; this._stopNow = true; }
    composedPath() { return this._path.slice(); }
    get returnValue() { return !this.defaultPrevented; }
    set returnValue(v) { if (!v) this.preventDefault(); }
    get cancelBubble() { return this._stop; }
    set cancelBubble(v) { if (v) this._stop = true; }
    get srcElement() { return this.target; }
    initEvent(type, bubbles, cancelable) { this.type = type; this.bubbles = !!bubbles; this.cancelable = !!cancelable; }
}
Event.NONE = 0; Event.CAPTURING_PHASE = 1; Event.AT_TARGET = 2; Event.BUBBLING_PHASE = 3;
class CustomEvent extends Event { constructor(t, i) { super(t, i); this.detail = i && i.detail !== undefined ? i.detail : null; } initCustomEvent(t, b, c, d) { this.initEvent(t, b, c); this.detail = d; } }
class UIEvent extends Event { constructor(t, i) { super(t, i); this.view = (i && i.view) || null; this.detail = (i && i.detail) || 0; } }
class MouseEvent extends UIEvent {
    constructor(t, i) {
        super(t, i); i = i || {};
        this.screenX = i.screenX || 0; this.screenY = i.screenY || 0;
        this.clientX = i.clientX || 0; this.clientY = i.clientY || 0;
        this.pageX = i.pageX !== undefined ? i.pageX : this.clientX; this.pageY = i.pageY !== undefined ? i.pageY : this.clientY;
        this.offsetX = this.clientX; this.offsetY = this.clientY; this.x = this.clientX; this.y = this.clientY;
        this.movementX = 0; this.movementY = 0;
        this.button = i.button || 0; this.buttons = i.buttons || 0;
        this.ctrlKey = !!i.ctrlKey; this.shiftKey = !!i.shiftKey; this.altKey = !!i.altKey; this.metaKey = !!i.metaKey;
        this.relatedTarget = i.relatedTarget || null;
    }
    getModifierState(k) { return (k === 'Shift' && this.shiftKey) || (k === 'Control' && this.ctrlKey) || (k === 'Alt' && this.altKey); }
}
class PointerEvent extends MouseEvent { constructor(t, i) { super(t, i); i = i || {}; this.pointerId = i.pointerId || 1; this.pointerType = i.pointerType || 'mouse'; this.isPrimary = true; this.width = 1; this.height = 1; this.pressure = 0; } }
class WheelEvent extends MouseEvent { constructor(t, i) { super(t, i); i = i || {}; this.deltaX = i.deltaX || 0; this.deltaY = i.deltaY || 0; this.deltaZ = 0; this.deltaMode = 0; } }
class KeyboardEvent extends UIEvent {
    constructor(t, i) {
        super(t, i); i = i || {};
        this.key = i.key || ''; this.code = i.code || ''; this.keyCode = i.keyCode || 0; this.which = this.keyCode;
        this.charCode = i.charCode || 0; this.location = 0; this.repeat = !!i.repeat; this.isComposing = false;
        this.ctrlKey = !!i.ctrlKey; this.shiftKey = !!i.shiftKey; this.altKey = !!i.altKey; this.metaKey = !!i.metaKey;
    }
    getModifierState(k) { return (k === 'Shift' && this.shiftKey) || (k === 'Control' && this.ctrlKey) || (k === 'Alt' && this.altKey); }
}
class FocusEvent extends UIEvent { constructor(t, i) { super(t, i); this.relatedTarget = (i && i.relatedTarget) || null; } }
class InputEvent extends UIEvent { constructor(t, i) { super(t, i); i = i || {}; this.data = i.data !== undefined ? i.data : null; this.inputType = i.inputType || ''; this.isComposing = false; } }
class CompositionEvent extends UIEvent { constructor(t, i) { super(t, i); this.data = (i && i.data) || ''; } }
class SubmitEvent extends Event { constructor(t, i) { super(t, i); this.submitter = (i && i.submitter) || null; } }
class ErrorEvent extends Event { constructor(t, i) { super(t, i); i = i || {}; this.message = i.message || ''; this.error = i.error; this.filename = i.filename || ''; this.lineno = 0; this.colno = 0; } }
class PopStateEvent extends Event { constructor(t, i) { super(t, i); this.state = i && i.state !== undefined ? i.state : null; } }
class HashChangeEvent extends Event { constructor(t, i) { super(t, i); i = i || {}; this.oldURL = i.oldURL || ''; this.newURL = i.newURL || ''; } }
class MessageEvent extends Event { constructor(t, i) { super(t, i); i = i || {}; this.data = i.data; this.origin = i.origin || ''; this.source = i.source || null; this.ports = i.ports || []; this.lastEventId = ''; } }
class ProgressEvent extends Event { constructor(t, i) { super(t, i); i = i || {}; this.loaded = i.loaded || 0; this.total = i.total || 0; this.lengthComputable = !!i.lengthComputable; } }
class StorageEvent extends Event { constructor(t, i) { super(t, i); i = i || {}; this.key = i.key || null; this.newValue = i.newValue || null; this.oldValue = i.oldValue || null; this.storageArea = i.storageArea || null; this.url = i.url || ''; } }
class AnimationEvent extends Event { constructor(t, i) { super(t, i); this.animationName = (i && i.animationName) || ''; this.elapsedTime = 0; } }
class TransitionEvent extends Event { constructor(t, i) { super(t, i); this.propertyName = (i && i.propertyName) || ''; this.elapsedTime = 0; } }
class TouchEvent extends UIEvent { constructor(t, i) { super(t, i); this.touches = []; this.targetTouches = []; this.changedTouches = []; } }
class PageTransitionEvent extends Event { constructor(t, i) { super(t, i); this.persisted = false; } }
Object.assign(G, { Event, CustomEvent, UIEvent, MouseEvent, PointerEvent, WheelEvent, KeyboardEvent, FocusEvent, InputEvent,
    CompositionEvent, SubmitEvent, ErrorEvent, PopStateEvent, HashChangeEvent, MessageEvent, ProgressEvent, StorageEvent,
    AnimationEvent, TransitionEvent, TouchEvent, PageTransitionEvent });

const LISTENERS = Symbol('listeners');
function listenersOf(t, create) {
    let m = t[LISTENERS];
    if (!m && create) { m = new Map(); val(t, LISTENERS, m); }
    return m;
}
function addEventListener(type, cb, opts) {
    if (!cb) return;
    const capture = typeof opts === 'boolean' ? opts : !!(opts && opts.capture);
    const once = !!(opts && typeof opts === 'object' && opts.once);
    const m = listenersOf(this, true);
    let list = m.get(type);
    if (!list) { list = []; m.set(type, list); }
    for (const l of list) if (l.cb === cb && l.capture === capture) return;
    const entry = { cb, capture, once, removed: false };
    list.push(entry);
    const signal = opts && typeof opts === 'object' && opts.signal;
    if (signal) signal.addEventListener('abort', () => this.removeEventListener(type, cb, opts));
}
function removeEventListener(type, cb, opts) {
    const capture = typeof opts === 'boolean' ? opts : !!(opts && opts.capture);
    const m = listenersOf(this, false);
    const list = m && m.get(type);
    if (!list) return;
    for (let i = 0; i < list.length; i++) {
        if (list[i].cb === cb && list[i].capture === capture) { list[i].removed = true; list.splice(i, 1); return; }
    }
}
function inlineHandler(t, type) {
    const prop = t['on' + type];
    if (typeof prop === 'function') return prop;
    if (t.nodeType === 1 && typeof t.getAttribute === 'function') {
        const code = t.getAttribute('on' + type);
        if (code) {
            try {
                const fn = new Function('event', code);
                return function (e) { return fn.call(t, e); };
            } catch (e) { reportError(e); }
        }
    }
    return null;
}
function invoke(t, ev, phase) {
    ev.currentTarget = t;
    const m = listenersOf(t, false);
    const list = m && m.get(ev.type);
    if (list) {
        for (const l of list.slice()) {
            if (l.removed || (phase === 1 && !l.capture) || (phase === 3 && l.capture)) continue;
            if (l.once) removeEventListener.call(t, ev.type, l.cb, l.capture);
            try {
                if (typeof l.cb === 'function') l.cb.call(t, ev);
                else if (l.cb && typeof l.cb.handleEvent === 'function') l.cb.handleEvent(ev);
            } catch (e) { reportError(e); }
            if (ev._stopNow) return;
        }
    }
    if (phase !== 1) {
        const h = inlineHandler(t, ev.type);
        if (h) {
            try {
                const r = h.call(t, ev);
                if (r === false) ev.preventDefault();
            } catch (e) { reportError(e); }
        }
    }
}
function dispatchEvent(ev) {
    const target = this;
    ev.target = target;
    const path = [];
    for (let n = target; n; n = n.parentNode) path.push(n);
    if (path.length && path[path.length - 1] === doc && ev.type !== 'load') path.push(G);
    ev._path = path;
    for (let i = path.length - 1; i > 0 && !ev._stop; i--) { ev.eventPhase = 1; invoke(path[i], ev, 1); }
    if (!ev._stop) { ev.eventPhase = 2; invoke(target, ev, 2); }
    if (ev.bubbles) for (let i = 1; i < path.length && !ev._stop; i++) { ev.eventPhase = 3; invoke(path[i], ev, 3); }
    ev.eventPhase = 0;
    ev.currentTarget = null;
    return !ev.defaultPrevented;
}
for (const proto of [EventTarget.prototype]) {
    val(proto, 'addEventListener', addEventListener);
    val(proto, 'removeEventListener', removeEventListener);
    val(proto, 'dispatchEvent', dispatchEvent);
}
G.addEventListener = addEventListener;
G.removeEventListener = removeEventListener;
G.dispatchEvent = function (ev) {
    ev.target = G; ev._path = [G]; ev.eventPhase = 2;
    invoke(G, ev, 2);
    ev.eventPhase = 0;
    return !ev.defaultPrevented;
};
/* `new EventTarget()` for libraries that use it as an event bus */
{
    const ETP = EventTarget.prototype;
    G.EventTarget = function EventTarget() {};
    G.EventTarget.prototype = ETP;
}

const KEY_NAMES = { 8: 'Backspace', 9: 'Tab', 13: 'Enter', 27: 'Escape', 32: ' ', 0x100: 'ArrowUp', 0x101: 'ArrowDown',
    0x102: 'ArrowLeft', 0x103: 'ArrowRight', 0x104: 'Delete', 0x105: 'Home', 0x106: 'End', 0x107: 'PageUp',
    0x108: 'PageDown', 0x109: 'Insert' };
const KEY_CODES = { Backspace: 8, Tab: 9, Enter: 13, Escape: 27, ' ': 32, ArrowUp: 38, ArrowDown: 40, ArrowLeft: 37,
    ArrowRight: 39, Delete: 46, Home: 36, End: 35, PageUp: 33, PageDown: 34, Insert: 45 };
const MOUSE_TYPES = new Set(['click', 'dblclick', 'mousedown', 'mouseup', 'mousemove', 'mouseover', 'mouseout',
    'mouseenter', 'mouseleave', 'contextmenu', 'auxclick']);
const NO_BUBBLE = new Set(['focus', 'blur', 'load', 'error', 'mouseenter', 'mouseleave', 'scroll', 'resize', 'abort']);

/* Called by the browser for user input and page lifecycle events. */
G.__dispatchFromHost = function (target, type, cx, cy, button, mods, key, px, py) {
    if (type === 'scroll' || type === 'resize') {
        const e = new Event(type);
        e.isTrusted = true;
        if (type === 'scroll') { e.target = doc; invoke(doc, e, 2); }
        G.dispatchEvent(new Event(type));
        return false;
    }
    const base = { bubbles: !NO_BUBBLE.has(type), cancelable: type !== 'input' && type !== 'focus' && type !== 'blur' && type !== 'load',
        view: G, shiftKey: !!(mods & 1), altKey: !!(mods & 2), ctrlKey: !!(mods & 4), metaKey: false };
    let ev;
    if (MOUSE_TYPES.has(type)) {
        Object.assign(base, { clientX: cx, clientY: cy, pageX: px, pageY: py, screenX: cx, screenY: cy, button,
            buttons: type === 'mousedown' ? 1 : 0, detail: type === 'click' ? 1 : type === 'dblclick' ? 2 : 0 });
        ev = new MouseEvent(type, base);
        if (type === 'mousedown' || type === 'mouseup') {
            const pt = new PointerEvent(type === 'mousedown' ? 'pointerdown' : 'pointerup', base);
            pt.isTrusted = true;
            target.dispatchEvent(pt);
        }
    } else if (type === 'keydown' || type === 'keyup' || type === 'keypress') {
        const name = KEY_NAMES[key] || (key >= 32 && key < 0x100 ? String.fromCharCode(key) : '');
        const code = KEY_CODES[name] || (name.length === 1 ? name.toUpperCase().charCodeAt(0) : 0);
        Object.assign(base, { key: name, code: name.length === 1 ? (/[a-z]/i.test(name) ? 'Key' + name.toUpperCase() : /[0-9]/.test(name) ? 'Digit' + name : name) : name,
            keyCode: code, charCode: type === 'keypress' ? key : 0 });
        ev = new KeyboardEvent(type, base);
    } else if (type === 'input' || type === 'beforeinput') {
        ev = new InputEvent(type, Object.assign(base, { inputType: 'insertText' }));
    } else if (type === 'focus' || type === 'blur' || type === 'focusin' || type === 'focusout') {
        ev = new FocusEvent(type, base);
    } else if (type === 'submit') {
        ev = new SubmitEvent(type, base);
    } else if (type === 'wheel') {
        ev = new WheelEvent(type, Object.assign(base, { deltaY: py, clientX: cx, clientY: cy }));
    } else {
        ev = new Event(type, base);
    }
    ev.isTrusted = true;
    target.dispatchEvent(ev);
    return ev.defaultPrevented;
};
G.__fireWindowLoad = function () {
    const ev = new Event('load');
    ev.isTrusted = true;
    G.dispatchEvent(ev);
    G.dispatchEvent(new PageTransitionEvent('pageshow'));
};

/* ---- Node --------------------------------------------------------------- */
const NP = Node.prototype, EP = Element.prototype, DP = Document.prototype;
Object.assign(Node, { ELEMENT_NODE: 1, ATTRIBUTE_NODE: 2, TEXT_NODE: 3, CDATA_SECTION_NODE: 4, PROCESSING_INSTRUCTION_NODE: 7,
    COMMENT_NODE: 8, DOCUMENT_NODE: 9, DOCUMENT_TYPE_NODE: 10, DOCUMENT_FRAGMENT_NODE: 11,
    DOCUMENT_POSITION_DISCONNECTED: 1, DOCUMENT_POSITION_PRECEDING: 2, DOCUMENT_POSITION_FOLLOWING: 4,
    DOCUMENT_POSITION_CONTAINS: 8, DOCUMENT_POSITION_CONTAINED_BY: 16 });
for (const k of Object.keys(Node)) if (/^[A-Z_]+$/.test(k)) NP[k] = Node[k];

function childArray(n, elementsOnly) {
    const out = [];
    for (let c = n.firstChild; c; c = c.nextSibling) if (!elementsOnly || c.nodeType === 1) out.push(c);
    return out;
}
function nodeList(arr) {
    val(arr, 'item', function (i) { return this[i] || null; });
    return arr;
}
def(NP, 'childNodes', function () { return nodeList(childArray(this, false)); });
def(NP, 'ownerDocument', function () { return this === doc ? null : doc; });
def(NP, 'baseURI', function () { return doc.baseURI; });
def(NP, 'nodeValue', function () { return null; }, function () {});
val(NP, 'getRootNode', function () { let n = this; while (n.parentNode) n = n.parentNode; return n; });
val(NP, 'isSameNode', function (o) { return this === o; });
val(NP, 'isEqualNode', function (o) { return !!o && this.nodeType === o.nodeType && (this.outerHTML || this.textContent) === (o.outerHTML || o.textContent); });
val(NP, 'normalize', function () {});
val(NP, 'lookupNamespaceURI', function () { return XHTML_NS; });
val(NP, 'compareDocumentPosition', function (other) {
    if (this === other) return 0;
    if (this.contains(other)) return 20;
    if (other.contains(this)) return 10;
    if (this.getRootNode() !== other.getRootNode()) return 33;
    const all = doc.querySelectorAll('*');
    const a = all.indexOf(this.nodeType === 1 ? this : this.parentNode), b = all.indexOf(other.nodeType === 1 ? other : other.parentNode);
    return a < b ? 4 : 2;
});

function toNode(x) { return x && typeof x === 'object' && typeof x.nodeType === 'number' ? x : doc.createTextNode(String(x)); }
function toFragment(args) {
    if (args.length === 1) return toNode(args[0]);
    const f = doc.createDocumentFragment();
    for (const a of args) f.appendChild(toNode(a));
    return f;
}
const ParentMixin = {
    append(...nodes) { this.appendChild(toFragment(nodes)); },
    prepend(...nodes) { this.insertBefore(toFragment(nodes), this.firstChild); },
    replaceChildren(...nodes) { while (this.firstChild) this.removeChild(this.firstChild); if (nodes.length) this.append(...nodes); },
    getElementsByTagName(name) { return this.querySelectorAll(name === '*' ? '*' : String(name).toLowerCase().replace(/[^a-z0-9_-]/g, '')); },
    getElementsByClassName(names) {
        const sel = String(names).trim().split(/\s+/).filter(Boolean).map((c) => '.' + CSS.escape(c)).join('');
        return sel ? this.querySelectorAll(sel) : nodeList([]);
    }
};
const ChildMixin = {
    before(...nodes) { if (this.parentNode) this.parentNode.insertBefore(toFragment(nodes), this); },
    after(...nodes) { if (this.parentNode) this.parentNode.insertBefore(toFragment(nodes), this.nextSibling); },
    replaceWith(...nodes) { if (this.parentNode) this.parentNode.replaceChild(toFragment(nodes), this); },
    remove() { this.__remove(); }
};
for (const proto of [EP, DocumentFragment.prototype, DP]) {
    for (const k of Object.keys(ParentMixin)) val(proto, k, ParentMixin[k]);
    def(proto, 'children', function () { return nodeList(childArray(this, true)); });
    def(proto, 'childElementCount', function () { return childArray(this, true).length; });
    def(proto, 'firstElementChild', function () { let c = this.firstChild; while (c && c.nodeType !== 1) c = c.nextSibling; return c; });
    def(proto, 'lastElementChild', function () { let c = this.lastChild; while (c && c.nodeType !== 1) c = c.previousSibling; return c; });
}
for (const proto of [EP, CharacterData.prototype]) {
    for (const k of Object.keys(ChildMixin)) val(proto, k, ChildMixin[k]);
    def(proto, 'nextElementSibling', function () { let c = this.nextSibling; while (c && c.nodeType !== 1) c = c.nextSibling; return c; });
    def(proto, 'previousElementSibling', function () { let c = this.previousSibling; while (c && c.nodeType !== 1) c = c.previousSibling; return c; });
}
def(CharacterData.prototype, 'length', function () { return this.data.length; });
val(CharacterData.prototype, 'appendData', function (s) { this.data += s; });
val(CharacterData.prototype, 'substringData', function (o, n) { return this.data.substr(o, n); });
def(Text.prototype, 'wholeText', function () { return this.data; });
val(Text.prototype, 'splitText', function (o) {
    const rest = doc.createTextNode(this.data.slice(o));
    this.data = this.data.slice(0, o);
    if (this.parentNode) this.parentNode.insertBefore(rest, this.nextSibling);
    return rest;
});

/* ---- Element ------------------------------------------------------------ */
const SVG_TAGS = new Set(['svg', 'path', 'g', 'circle', 'rect', 'line', 'polyline', 'polygon', 'ellipse', 'text', 'tspan',
    'defs', 'use', 'symbol', 'clippath', 'mask', 'lineargradient', 'radialgradient', 'stop', 'pattern', 'foreignobject',
    'marker', 'filter', 'fegaussianblur', 'feoffset', 'feblend', 'fecolormatrix', 'desc', 'title', 'image', 'switch', 'view']);
def(EP, 'namespaceURI', function () {
    const n = this.localName;
    if (n === 'svg') return SVG_NS;
    if (SVG_TAGS.has(n) && n !== 'title' && n !== 'image') return SVG_NS;
    for (let p = this.parentNode; p && p.nodeType === 1; p = p.parentNode) if (p.localName === 'svg') return SVG_NS;
    return XHTML_NS;
});
def(EP, 'prefix', () => null);
def(EP, 'id', function () { return this.getAttribute('id') || ''; }, function (v) { this.setAttribute('id', v); });
def(EP, 'className', function () { return this.getAttribute('class') || ''; }, function (v) { this.setAttribute('class', v); });
val(EP, 'hasAttribute', function (n) { return this.getAttribute(n) !== null; });
val(EP, 'hasAttributes', function () { return this.getAttributeNames().length > 0; });
val(EP, 'toggleAttribute', function (n, force) {
    const has = this.hasAttribute(n);
    if (force === true || (force === undefined && !has)) { if (!has) this.setAttribute(n, ''); return true; }
    if (has) this.removeAttribute(n);
    return false;
});
val(EP, 'getAttributeNS', function (ns, n) { return this.getAttribute(n); });
val(EP, 'setAttributeNS', function (ns, n, v) { this.setAttribute(String(n).replace(/^.*:/, ''), v); });
val(EP, 'removeAttributeNS', function (ns, n) { this.removeAttribute(n); });
val(EP, 'hasAttributeNS', function (ns, n) { return this.hasAttribute(n); });
def(EP, 'attributes', function () {
    const el = this;
    const list = this.getAttributeNames().map((name) => ({ name, localName: name, value: el.getAttribute(name), nodeName: name, nodeValue: el.getAttribute(name), specified: true, namespaceURI: null }));
    val(list, 'item', (i) => list[i] || null);
    val(list, 'getNamedItem', (n) => list.find((a) => a.name === String(n).toLowerCase()) || null);
    for (const a of list) if (!(a.name in list)) list[a.name] = a;
    return list;
});
val(EP, 'closest', function (sel) {
    for (let n = this; n && n.nodeType === 1; n = n.parentNode) if (n.matches(sel)) return n;
    return null;
});
EP.webkitMatchesSelector = EP.msMatchesSelector = EP.matches;
val(EP, 'insertAdjacentElement', function (where, el) {
    switch (String(where).toLowerCase()) {
    case 'beforebegin': if (this.parentNode) this.parentNode.insertBefore(el, this); break;
    case 'afterbegin': this.insertBefore(el, this.firstChild); break;
    case 'beforeend': this.appendChild(el); break;
    case 'afterend': if (this.parentNode) this.parentNode.insertBefore(el, this.nextSibling); break;
    }
    return el;
});
val(EP, 'insertAdjacentText', function (where, text) { this.insertAdjacentElement(where, doc.createTextNode(text)); });
def(EP, 'innerText', function () { return this.textContent; }, function (v) { this.textContent = v; });
def(EP, 'outerText', function () { return this.textContent; });
def(EP, 'shadowRoot', () => null);
val(EP, 'attachShadow', function () {
    /* no shadow DOM yet: a component renders into its light DOM instead */
    val(this, 'shadowRoot', this);
    return this;
});
val(EP, 'getClientRects', function () { return [this.getBoundingClientRect()]; });
val(EP, 'scrollIntoView', function () {
    const r = this.getBoundingClientRect();
    __scrollTo(0, r.top + G.scrollY);
});
val(EP, 'scroll', function () {}); val(EP, 'scrollTo', function () {}); val(EP, 'scrollBy', function () {});
val(EP, 'animate', function () {
    const a = { finished: Promise.resolve(), cancel() {}, finish() {}, play() {}, pause() {}, reverse() {}, onfinish: null, playState: 'finished' };
    a.finished.then(() => { if (a.onfinish) a.onfinish(); });
    return a;
});
val(EP, 'getAnimations', function () { return []; });
val(EP, 'requestFullscreen', function () { return Promise.reject(new Error('not supported')); });
val(EP, 'setPointerCapture', function () {}); val(EP, 'releasePointerCapture', function () {});
val(EP, 'hasPointerCapture', function () { return false; });
for (const [name, prop] of [['offsetWidth', 'width'], ['offsetHeight', 'height'], ['clientWidth', 'width'], ['clientHeight', 'height'],
    ['scrollWidth', 'width'], ['scrollHeight', 'height']]) {
    def(EP, name, function () {
        if (this === doc.documentElement && (name === 'clientWidth' || name === 'clientHeight')) return name === 'clientWidth' ? G.innerWidth : G.innerHeight;
        return Math.round(this.getBoundingClientRect()[prop]);
    });
}
def(EP, 'offsetTop', function () { return Math.round(this.getBoundingClientRect().top + G.scrollY); });
def(EP, 'offsetLeft', function () { return Math.round(this.getBoundingClientRect().left); });
def(EP, 'offsetParent', function () { return this.parentNode && this.parentNode.nodeType === 1 ? doc.body : null; });
def(EP, 'clientTop', () => 0); def(EP, 'clientLeft', () => 0);
def(EP, 'scrollTop', function () { return this === doc.documentElement || this === doc.body ? G.scrollY : 0; },
    function (v) { if (this === doc.documentElement || this === doc.body) __scrollTo(0, +v); });
def(EP, 'scrollLeft', () => 0, () => {});

/* reflected attributes.  They sit on every element (browsers spread them
 * over the HTML*Element interfaces), so a component class assigning
 * `MyElement.prototype.async = function () {}` lands on one: on anything
 * that is not a DOM node the setter makes a plain property and the getter
 * gives the default. */
const notNode = (o) => { try { return typeof o.nodeType !== 'number'; } catch (e) { return true; } };
const ownProp = (o, prop, v) => Object.defineProperty(o, prop, { value: v, writable: true, enumerable: true, configurable: true });
const reflectStr = (proto, prop, attr) => def(proto, prop, function () {
    if (notNode(this)) return '';
    const v = this.getAttribute(attr || prop.toLowerCase()); return v === null ? '' : v;
}, function (v) { if (notNode(this)) ownProp(this, prop, v); else this.setAttribute(attr || prop.toLowerCase(), v); });
const reflectBool = (proto, prop, attr) => def(proto, prop, function () { return notNode(this) ? false : this.hasAttribute(attr || prop.toLowerCase()); },
    function (v) { if (notNode(this)) ownProp(this, prop, v); else this.toggleAttribute(attr || prop.toLowerCase(), !!v); });
const reflectUrl = (proto, prop) => def(proto, prop, function () {
    if (notNode(this)) return '';
    const v = this.getAttribute(prop);
    if (v === null) return '';
    return __resolve(doc.baseURI, v) || v;
}, function (v) { if (notNode(this)) ownProp(this, prop, v); else this.setAttribute(prop, v); });
for (const p of ['title', 'lang', 'dir', 'name', 'type', 'alt', 'placeholder', 'rel', 'target', 'download', 'accept', 'autocomplete',
    'enctype', 'inputMode', 'role', 'slot', 'pattern', 'min', 'max', 'step', 'crossOrigin', 'referrerPolicy', 'loading', 'decoding',
    'media', 'sizes', 'srcset', 'width', 'height', 'label', 'nonce', 'integrity', 'as', 'charset', 'content', 'httpEquiv']) {
    reflectStr(EP, p, p === 'httpEquiv' ? 'http-equiv' : p.toLowerCase());
}
reflectStr(EP, 'htmlFor', 'for');
reflectStr(EP, 'defaultValue', 'value');
reflectStr(EP, 'method');
for (const p of ['hidden', 'disabled', 'readOnly', 'required', 'multiple', 'autofocus', 'async', 'defer', 'noModule',
    'open', 'controls', 'autoplay', 'loop', 'muted', 'defaultChecked', 'noValidate', 'formNoValidate', 'draggable', 'isMap']) {
    reflectBool(EP, p, p === 'defaultChecked' ? 'checked' : p.toLowerCase());
}
def(EP, 'tabIndex', function () { const v = this.getAttribute('tabindex'); return v === null ? (/^(a|button|input|select|textarea)$/.test(this.localName) ? 0 : -1) : (v | 0); },
    function (v) { this.setAttribute('tabindex', String(v)); });
for (const p of ['href', 'src', 'action', 'cite', 'poster', 'formAction']) reflectUrl(EP, p);
def(EP, 'contentEditable', function () { return this.getAttribute('contenteditable') || 'inherit'; }, function (v) { this.setAttribute('contenteditable', v); });
def(EP, 'isContentEditable', function () { const v = this.getAttribute('contenteditable'); return v === '' || v === 'true'; });
def(EP, 'accessKey', () => '', () => {});
def(EP, 'spellcheck', () => true, () => {});
def(EP, 'translate', () => true, () => {});
def(EP, 'inert', function () { return this.hasAttribute('inert'); }, function (v) { this.toggleAttribute('inert', !!v); });
def(EP, 'selected', function () {
    const sel = this.closest('select');
    return !!sel && sel.options[sel.selectedIndex] === this;
}, function (v) {
    const sel = this.closest('select');
    if (sel && v) sel.selectedIndex = sel.options.indexOf(this);
});
def(EP, 'defaultSelected', function () { return this.hasAttribute('selected'); }, function (v) { this.toggleAttribute('selected', !!v); });
def(EP, 'index', function () { const s = this.closest('select'); return s ? s.options.indexOf(this) : 0; });
def(EP, 'text', function () { return this.textContent; }, function (v) { this.textContent = v; });
def(EP, 'options', function () { return nodeList(this.__options()); });
def(EP, 'selectedOptions', function () { const o = this.options[this.selectedIndex]; return nodeList(o ? [o] : []); });
def(EP, 'length', function () { return this.localName === 'select' ? this.__options().length : this.localName === 'form' ? this.elements.length : undefined; });
val(EP, 'add', function (opt, before) { this.insertBefore(opt, before && before.nodeType ? before : null); });
def(EP, 'form', function () {
    const id = this.getAttribute('form');
    if (id) return doc.getElementById(id);
    let p = this.parentNode;
    while (p && p.localName !== 'form') p = p.parentNode;
    return p && p.nodeType === 1 ? p : null;
});
def(EP, 'elements', function () {
    const form = this;
    const list = Array.from(doc.querySelectorAll('input,select,textarea,button,fieldset,output,object')).filter((e) => e.form === form);
    for (const e of list) { const n = e.getAttribute('name') || e.getAttribute('id'); if (n && !(n in list)) list[n] = e; }
    val(list, 'namedItem', (n) => list.find((e) => e.getAttribute('name') === n || e.id === n) || null);
    return nodeList(list);
});
val(EP, 'submit', function () { __submit(this, null); });
val(EP, 'requestSubmit', function (submitter) {
    const ev = new SubmitEvent('submit', { bubbles: true, cancelable: true, submitter: submitter || null });
    if (this.dispatchEvent(ev)) __submit(this, submitter || null);
});
val(EP, 'reset', function () {
    for (const e of this.elements) {
        if (e.type === 'checkbox' || e.type === 'radio') e.checked = e.defaultChecked;
        else if (e.localName !== 'button') e.value = e.defaultValue;
    }
});
val(EP, 'checkValidity', function () { return true; });
val(EP, 'reportValidity', function () { return true; });
val(EP, 'setCustomValidity', function () {});
def(EP, 'validity', () => ({ valid: true, valueMissing: false, typeMismatch: false, patternMismatch: false, tooLong: false,
    tooShort: false, rangeUnderflow: false, rangeOverflow: false, stepMismatch: false, badInput: false, customError: false }));
def(EP, 'validationMessage', () => '');
def(EP, 'willValidate', () => true);
def(EP, 'labels', function () { const id = this.id; return nodeList(id ? doc.querySelectorAll('label[for="' + id + '"]') : []); });
val(EP, 'select', function () {});
val(EP, 'setSelectionRange', function () {});
val(EP, 'setRangeText', function () {});
def(EP, 'selectionStart', function () { return (this.value || '').length; }, () => {});
def(EP, 'selectionEnd', function () { return (this.value || '').length; }, () => {});
def(EP, 'selectionDirection', () => 'none', () => {});
def(EP, 'valueAsNumber', function () { return parseFloat(this.value); }, function (v) { this.value = String(v); });
def(EP, 'files', () => nodeList([]));
def(EP, 'indeterminate', () => false, () => {});
/* media and canvas stand-ins */
val(EP, 'play', function () { return Promise.resolve(); });
val(EP, 'pause', function () {});
val(EP, 'load', function () {});
val(EP, 'canPlayType', function () { return ''; });
def(EP, 'paused', () => true);
def(EP, 'currentTime', () => 0, () => {});
def(EP, 'duration', () => NaN);
val(EP, 'getContext', function () { return null; });
val(EP, 'toDataURL', function () { return 'data:,'; });
def(EP, 'naturalWidth', function () { return this.getBoundingClientRect().width | 0; });
def(EP, 'naturalHeight', function () { return this.getBoundingClientRect().height | 0; });
def(EP, 'complete', () => true);
def(EP, 'currentSrc', function () { return this.src; });
val(EP, 'decode', function () { return Promise.resolve(); });
def(EP, 'contentWindow', () => null);
def(EP, 'contentDocument', () => null);
/* A template's content is one DocumentFragment that lives as long as the
 * template; the parser puts the children in the template itself, so they
 * move into it on access (also after innerHTML is set again). */
const templateContent = new WeakMap();
def(EP, 'content', function () {
    if (notNode(this)) return undefined;
    if (this.localName !== 'template') { const v = this.getAttribute('content'); return v === null ? '' : v; }   /* <meta content> */
    let f = templateContent.get(this);
    if (!f) {
        f = doc.createDocumentFragment();
        templateContent.set(this, f);
    }
    if (this.firstChild) {
        if (f.firstChild) while (f.firstChild) f.removeChild(f.firstChild);   /* innerHTML replaced it */
        while (this.firstChild) f.appendChild(this.firstChild);
    }
    return f;
}, function (v) {
    if (notNode(this)) Object.defineProperty(this, 'content', { value: v, writable: true, enumerable: true, configurable: true });
    else if (this.localName !== 'template') this.setAttribute('content', v);
});
val(EP, 'showModal', function () { this.setAttribute('open', ''); });
val(EP, 'show', function () { this.setAttribute('open', ''); });
val(EP, 'close', function () { this.removeAttribute('open'); this.dispatchEvent(new Event('close')); });
val(EP, 'click', function () {
    if (this.disabled) return;
    const ev = new MouseEvent('click', { bubbles: true, cancelable: true, view: G, detail: 1 });
    const ok = this.dispatchEvent(ev);
    if (!ok) return;
    const tag = this.localName, type = (this.getAttribute('type') || '').toLowerCase();
    if (tag === 'a' && this.hasAttribute('href')) {
        const href = this.getAttribute('href');
        if (!/^javascript:/i.test(href)) __navigate(href, false);
    } else if (tag === 'input' && (type === 'checkbox' || type === 'radio')) {
        this.checked = type === 'radio' ? true : !this.checked;
        this.dispatchEvent(new Event('input', { bubbles: true }));
        this.dispatchEvent(new Event('change', { bubbles: true }));
    } else if ((tag === 'button' && type !== 'button' && type !== 'reset') || (tag === 'input' && (type === 'submit' || type === 'image'))) {
        const form = this.form;
        if (form) form.requestSubmit(this);
    }
});

/* classList / relList: DOMTokenList over an attribute */
class DOMTokenList {
    constructor(el, attr) { val(this, '_el', el); val(this, '_attr', attr); }
    _get() { return (this._el.getAttribute(this._attr) || '').split(/\s+/).filter(Boolean); }
    _set(list) { this._el.setAttribute(this._attr, list.join(' ')); }
    get length() { return this._get().length; }
    get value() { return this._el.getAttribute(this._attr) || ''; }
    set value(v) { this._el.setAttribute(this._attr, v); }
    item(i) { return this._get()[i] || null; }
    contains(t) { return this._get().includes(String(t)); }
    add(...ts) { const l = this._get(); let ch = false; for (const raw of ts) { const t = String(raw); if (!l.includes(t)) { l.push(t); ch = true; } } if (ch || !this._el.hasAttribute(this._attr)) this._set(l); }
    remove(...ts) { const l = this._get(); const n = l.filter((x) => !ts.map(String).includes(x)); if (n.length !== l.length) this._set(n); }
    toggle(t, force) {
        t = String(t);
        const has = this.contains(t);
        if (force === true || (force === undefined && !has)) { if (!has) this.add(t); return true; }
        if (has) this.remove(t);
        return false;
    }
    replace(a, b) { const l = this._get(); const i = l.indexOf(String(a)); if (i < 0) return false; l[i] = String(b); this._set(l); return true; }
    supports(t) { return ['modulepreload', 'preload', 'stylesheet', 'icon', 'noopener', 'noreferrer', 'prefetch', 'dns-prefetch', 'preconnect'].includes(String(t).toLowerCase()); }
    forEach(cb, thisArg) { this._get().forEach(cb, thisArg); }
    entries() { return this._get().entries(); }
    keys() { return this._get().keys(); }
    values() { return this._get().values(); }
    toString() { return this.value; }
    [Symbol.iterator]() { return this._get()[Symbol.iterator](); }
}
G.DOMTokenList = DOMTokenList;
const CLASSLIST = Symbol('classList'), RELLIST = Symbol('relList');
def(EP, 'classList', function () { return this[CLASSLIST] || (val(this, CLASSLIST, new DOMTokenList(this, 'class')), this[CLASSLIST]); },
    function (v) { this.setAttribute('class', v); });
def(EP, 'relList', function () { return this[RELLIST] || (val(this, RELLIST, new DOMTokenList(this, 'rel')), this[RELLIST]); });

const kebab = (s) => String(s).replace(/[A-Z]/g, (c) => '-' + c.toLowerCase());
const camel = (s) => String(s).replace(/-([a-z])/g, (m, c) => c.toUpperCase());
const DATASET = Symbol('dataset');
def(EP, 'dataset', function () {
    if (this[DATASET]) return this[DATASET];
    const el = this;
    const ds = new Proxy({}, {
        get(t, k) { if (typeof k !== 'string') return undefined; const v = el.getAttribute('data-' + kebab(k)); return v === null ? undefined : v; },
        set(t, k, v) { el.setAttribute('data-' + kebab(k), String(v)); return true; },
        has(t, k) { return typeof k === 'string' && el.hasAttribute('data-' + kebab(k)); },
        deleteProperty(t, k) { el.removeAttribute('data-' + kebab(k)); return true; },
        ownKeys() { return el.getAttributeNames().filter((n) => n.startsWith('data-')).map((n) => camel(n.slice(5))); },
        getOwnPropertyDescriptor(t, k) {
            const v = el.getAttribute('data-' + kebab(k));
            return v === null ? undefined : { value: v, writable: true, enumerable: true, configurable: true };
        }
    });
    val(this, DATASET, ds);
    return ds;
});

/* style: a CSSStyleDeclaration over the style attribute */
function cssName(k) {
    if (k === 'cssFloat') return 'float';
    if (k.startsWith('--')) return k;
    if (/^(webkit|moz|ms|o)[A-Z]/.test(k)) k = k[0].toUpperCase() + k.slice(1);
    let n = kebab(k);
    if (/^(webkit|moz|ms|o)-/.test(n)) n = '-' + n;
    return n;
}
function parseDecls(text) {
    const out = [];
    if (!text) return out;
    let depth = 0, quote = '', start = 0;
    const push = (s) => {
        const i = s.indexOf(':');
        if (i < 0) return;
        const name = s.slice(0, i).trim(), rest = s.slice(i + 1).trim();
        if (!name) return;
        const imp = /!\s*important$/i.test(rest);
        out.push([name.startsWith('--') ? name : name.toLowerCase(), imp ? rest.replace(/!\s*important$/i, '').trim() : rest, imp]);
    };
    for (let i = 0; i < text.length; i++) {
        const c = text[i];
        if (quote) { if (c === quote) quote = ''; continue; }
        if (c === '"' || c === "'") quote = c;
        else if (c === '(') depth++;
        else if (c === ')') depth--;
        else if (c === ';' && depth === 0) { push(text.slice(start, i)); start = i + 1; }
    }
    push(text.slice(start));
    return out;
}
function writeDecls(el, decls) {
    el.setAttribute('style', decls.map(([n, v, imp]) => n + ': ' + v + (imp ? ' !important' : '')).join('; '));
}
const STYLE = Symbol('style');
function styleObject(el) {
    const api = {
        getPropertyValue(n) { const d = parseDecls(el.getAttribute('style')).find((x) => x[0] === n); return d ? d[1] : ''; },
        getPropertyPriority(n) { const d = parseDecls(el.getAttribute('style')).find((x) => x[0] === n); return d && d[2] ? 'important' : ''; },
        setProperty(n, v, prio) {
            n = String(n);
            if (!n.startsWith('--')) n = n.toLowerCase();
            const decls = parseDecls(el.getAttribute('style')).filter((x) => x[0] !== n);
            if (v !== null && v !== undefined && String(v) !== '') decls.push([n, String(v), prio === 'important']);
            writeDecls(el, decls);
        },
        removeProperty(n) {
            const decls = parseDecls(el.getAttribute('style'));
            const d = decls.find((x) => x[0] === n);
            writeDecls(el, decls.filter((x) => x[0] !== n));
            return d ? d[1] : '';
        },
        item(i) { const d = parseDecls(el.getAttribute('style'))[i]; return d ? d[0] : ''; },
        get length() { return parseDecls(el.getAttribute('style')).length; },
        get cssText() { return el.getAttribute('style') || ''; },
        set cssText(v) { el.setAttribute('style', v); },
        get parentRule() { return null; }
    };
    return new Proxy(api, {
        get(t, k) {
            if (k in t) return typeof t[k] === 'function' ? t[k].bind(t) : t[k];
            if (typeof k !== 'string') return undefined;
            if (/^\d+$/.test(k)) return t.item(+k);
            return t.getPropertyValue(cssName(k));
        },
        set(t, k, v) {
            if (k === 'cssText') { t.cssText = v; return true; }
            if (typeof k === 'string') t.setProperty(cssName(k), v === null || v === undefined ? '' : String(v));
            return true;
        },
        has(t, k) { return typeof k === 'string'; }
    });
}
def(EP, 'style', function () { return this[STYLE] || (val(this, STYLE, styleObject(this)), this[STYLE]); },
    function (v) { this.setAttribute('style', String(v)); });
G.CSSStyleDeclaration = function CSSStyleDeclaration() {};
G.CSS = {
    supports(prop, value) {
        const p = value === undefined ? String(prop) : prop + ':' + value;
        return !/(grid-template-areas|backdrop-filter|container|@)/.test(p);
    },
    escape(s) { return String(s).replace(/([^\w-])/g, '\\$1').replace(/^(\d)/, '\\3$1 '); }
};

/* ---- Document ----------------------------------------------------------- */
def(DP, 'readyState', () => G.__readyState || 'loading');
def(DP, 'currentScript', () => G.__currentScript || null);
def(DP, 'defaultView', () => G);
def(DP, 'location', () => G.location, (v) => { G.location.href = v; });
def(DP, 'URL', () => G.location.href);
def(DP, 'documentURI', () => G.location.href);
def(DP, 'baseURI', function () {
    const b = this.querySelector('base[href]');
    return b ? (__resolve(__getUrl(), b.getAttribute('href')) || __getUrl()) : __getUrl();
});
def(DP, 'domain', () => G.location.hostname, () => {});
def(DP, 'referrer', () => '');
def(DP, 'characterSet', () => 'UTF-8');
def(DP, 'charset', () => 'UTF-8');
def(DP, 'inputEncoding', () => 'UTF-8');
def(DP, 'contentType', () => 'text/html');
def(DP, 'compatMode', () => 'CSS1Compat');
def(DP, 'hidden', () => false);
def(DP, 'visibilityState', () => 'visible');
def(DP, 'doctype', () => ({ name: 'html', nodeType: 10 }));
def(DP, 'scrollingElement', function () { return this.documentElement; });
def(DP, 'styleSheets', function () { return Array.from(this.querySelectorAll('style,link[rel~=stylesheet]')).map((n) => ({ ownerNode: n, cssRules: [], rules: [], insertRule() { return 0; }, deleteRule() {}, disabled: false })); });
def(DP, 'fonts', () => FONTS);
def(DP, 'forms', function () { return this.querySelectorAll('form'); });
def(DP, 'images', function () { return this.querySelectorAll('img'); });
def(DP, 'links', function () { return this.querySelectorAll('a[href],area[href]'); });
def(DP, 'scripts', function () { return this.querySelectorAll('script'); });
def(DP, 'implementation', () => ({ hasFeature: () => true, createHTMLDocument: () => doc }));
const FONTS = { ready: Promise.resolve(), status: 'loaded', check: () => true, load: () => Promise.resolve([]), add() {}, delete() {},
    addEventListener() {}, removeEventListener() {}, forEach() {}, size: 0 };
val(DP, 'getElementsByName', function (n) { return this.querySelectorAll('[name="' + String(n).replace(/"/g, '\\"') + '"]'); });
val(DP, 'hasFocus', function () { return true; });
val(DP, 'createEvent', function (kind) {
    const k = String(kind).toLowerCase();
    if (k.startsWith('mouse')) return new MouseEvent('');
    if (k.startsWith('keyboard')) return new KeyboardEvent('');
    if (k.startsWith('custom')) return new CustomEvent('');
    return new Event('');
});
val(DP, 'createAttribute', function (n) { return { name: n, value: '' }; });
val(DP, 'importNode', function (n, deep) { return n.cloneNode(!!deep); });
val(DP, 'adoptNode', function (n) { if (n.parentNode) n.parentNode.removeChild(n); return n; });
val(DP, 'elementFromPoint', function () { return null; });
val(DP, 'elementsFromPoint', function () { return []; });
val(DP, 'getSelection', function () { return G.getSelection(); });
val(DP, 'execCommand', function () { return false; });
val(DP, 'queryCommandSupported', function () { return false; });
val(DP, 'open', function () { return this; });
val(DP, 'close', function () {});
val(DP, 'write', function (...parts) {
    const html = parts.join('');
    const s = G.__currentScript;
    const tmp = this.createElement('div');
    tmp.innerHTML = html;
    const frag = this.createDocumentFragment();
    while (tmp.firstChild) frag.appendChild(tmp.firstChild);
    if (s && s.parentNode) s.parentNode.insertBefore(frag, s.nextSibling);
    else this.body.appendChild(frag);
});
val(DP, 'writeln', function (...p) { this.write(...p, '\n'); });
val(DP, 'createRange', function () {
    return { setStart() {}, setEnd() {}, setStartBefore() {}, setEndAfter() {}, selectNode() {}, selectNodeContents() {}, collapse() {},
        commonAncestorContainer: doc.body, getBoundingClientRect: () => ({ x: 0, y: 0, top: 0, left: 0, width: 0, height: 0, right: 0, bottom: 0 }),
        getClientRects: () => [], cloneRange() { return this; }, detach() {},
        createContextualFragment(html) { const t = doc.createElement('div'); t.innerHTML = html; const f = doc.createDocumentFragment(); while (t.firstChild) f.appendChild(t.firstChild); return f; } };
});
val(DP, 'createTreeWalker', function (root, whatToShow) {
    const show = whatToShow === undefined ? 0xFFFFFFFF : whatToShow;
    const ok = (n) => (n.nodeType === 1 && (show & 1)) || (n.nodeType === 3 && (show & 4)) || (n.nodeType === 8 && (show & 128));
    const w = {
        root, currentNode: root,
        nextNode() {
            let n = this.currentNode;
            for (;;) {
                if (n.firstChild) n = n.firstChild;
                else {
                    while (n && n !== root && !n.nextSibling) n = n.parentNode;
                    if (!n || n === root) return null;
                    n = n.nextSibling;
                }
                if (ok(n)) { this.currentNode = n; return n; }
            }
        },
        parentNode() { const p = this.currentNode.parentNode; if (p && p !== root) { this.currentNode = p; return p; } return null; },
        firstChild() { let c = this.currentNode.firstChild; while (c && !ok(c)) c = c.nextSibling; if (c) this.currentNode = c; return c || null; },
        nextSibling() { let c = this.currentNode.nextSibling; while (c && !ok(c)) c = c.nextSibling; if (c) this.currentNode = c; return c || null; }
    };
    return w;
});
G.NodeFilter = { SHOW_ALL: 0xFFFFFFFF, SHOW_ELEMENT: 1, SHOW_TEXT: 4, SHOW_COMMENT: 128, FILTER_ACCEPT: 1, FILTER_REJECT: 2, FILTER_SKIP: 3 };

/* ---- window ------------------------------------------------------------- */
def(G, 'innerWidth', () => __viewport()[0]);
def(G, 'innerHeight', () => __viewport()[1]);
def(G, 'outerWidth', () => __viewport()[0]);
def(G, 'outerHeight', () => __viewport()[1] + 80);
def(G, 'scrollX', () => 0);
def(G, 'scrollY', () => __viewport()[2]);
def(G, 'pageXOffset', () => 0);
def(G, 'pageYOffset', () => __viewport()[2]);
G.screenX = 0; G.screenY = 0; G.screenLeft = 0; G.screenTop = 0;
G.devicePixelRatio = 1;
G.screen = { get width() { return __viewport()[0]; }, get height() { return __viewport()[1]; }, get availWidth() { return __viewport()[0]; },
    get availHeight() { return __viewport()[1]; }, colorDepth: 24, pixelDepth: 24, orientation: { type: 'landscape-primary', angle: 0, addEventListener() {} } };
G.scrollTo = G.scroll = function (x, y) {
    if (x && typeof x === 'object') { y = x.top; x = x.left; }
    __scrollTo(+x || 0, +y || 0);
};
G.scrollBy = function (x, y) {
    if (x && typeof x === 'object') { y = x.top; x = x.left; }
    __scrollTo(0, G.scrollY + (+y || 0));
};
G.alert = (m) => { __status(String(m)); };
G.confirm = (m) => { __status(String(m)); return true; };
G.prompt = (m, d) => { __status(String(m)); return d === undefined ? null : String(d); };
G.print = () => {};
G.focus = () => {};
G.blur = () => {};
G.stop = () => {};
G.open = (url) => { if (url) __navigate(String(url), false); return null; };
G.close = () => {};
G.moveTo = G.moveBy = G.resizeTo = G.resizeBy = () => {};
G.getSelection = () => ({ rangeCount: 0, isCollapsed: true, type: 'None', anchorNode: null, focusNode: null, toString: () => '',
    removeAllRanges() {}, addRange() {}, getRangeAt() { return doc.createRange(); }, collapse() {}, empty() {} });
G.postMessage = function (data) {
    setTimeout(() => G.dispatchEvent(new MessageEvent('message', { data, origin: G.location.origin, source: G })), 0);
};
G.getComputedStyle = function (el, pseudo) {
    const c = el && el.__computed ? el.__computed() : {};
    const inline = el && el.style;
    return new Proxy({}, {
        get(t, k) {
            if (k === 'getPropertyValue') return (n) => { const ck = camel(n); return c[ck] !== undefined ? c[ck] : (inline ? inline.getPropertyValue(n) : ''); };
            if (k === 'getPropertyPriority') return () => '';
            if (typeof k !== 'string') return undefined;
            if (c[k] !== undefined) return c[k];
            if (k === 'length') return 0;
            return inline ? inline[k] : '';
        }
    });
};
function evalMedia(q) {
    q = String(q).toLowerCase().trim();
    if (!q || q === 'all' || q === 'screen') return true;
    if (q === 'print') return false;
    return q.split(',').some((part) => part.split(/\band\b/).every((cond) => {
        cond = cond.trim().replace(/^only\s+/, '');
        if (cond === 'screen' || cond === 'all' || cond === '') return true;
        if (cond === 'print' || cond.startsWith('not ')) return false;
        const m = /^\(\s*([a-z-]+)\s*(?::\s*([^)]+))?\)$/.exec(cond);
        if (!m) return false;
        const f = m[1], v = (m[2] || '').trim(), n = parseFloat(v) * (v.endsWith('em') ? 16 : 1);
        switch (f) {
        case 'min-width': return G.innerWidth >= n;
        case 'max-width': return G.innerWidth <= n;
        case 'min-height': return G.innerHeight >= n;
        case 'max-height': return G.innerHeight <= n;
        case 'prefers-color-scheme': return v === 'light';
        case 'prefers-reduced-motion': return v === 'reduce';
        case 'prefers-contrast': return v === 'no-preference';
        case 'hover': return v === 'hover' || !v;
        case 'pointer': return v === 'fine';
        case 'any-hover': return v === 'hover';
        case 'any-pointer': return v === 'fine';
        case 'orientation': return v === (G.innerWidth >= G.innerHeight ? 'landscape' : 'portrait');
        case 'display-mode': return v === 'browser';
        case 'min-resolution': case 'max-resolution': case '-webkit-min-device-pixel-ratio': return parseFloat(v) <= 1;
        default: return false;
        }
    }));
}
G.matchMedia = function (q) {
    return { matches: evalMedia(q), media: String(q), onchange: null, addListener() {}, removeListener() {},
        addEventListener() {}, removeEventListener() {}, dispatchEvent() { return true; } };
};

/* ---- URL ---------------------------------------------------------------- */
const URL_RE = /^([a-z][a-z0-9+.-]*:)(?:\/\/(?:([^:@\/?#]*)(?::([^@\/?#]*))?@)?(\[[^\]]+\]|[^:\/?#]*)(?::(\d*))?)?([^?#]*)(\?[^#]*)?(#.*)?$/i;
const DEFAULT_PORT = { 'http:': '80', 'https:': '443', 'ws:': '80', 'wss:': '443', 'ftp:': '21' };
class URLSearchParams {
    constructor(init) {
        val(this, '_l', []);
        val(this, '_u', null);
        if (init === undefined || init === null) return;
        if (typeof init === 'object' && !(init instanceof String)) {
            if (init instanceof URLSearchParams || Array.isArray(init) || typeof init[Symbol.iterator] === 'function') {
                for (const [k, v] of init) this._l.push([String(k), String(v)]);
            } else {
                for (const k of Object.keys(init)) this._l.push([k, String(init[k])]);
            }
            return;
        }
        let s = String(init);
        if (s[0] === '?') s = s.slice(1);
        for (const part of s.split('&')) {
            if (!part) continue;
            const i = part.indexOf('=');
            const dec = (x) => { try { return decodeURIComponent(x.replace(/\+/g, ' ')); } catch (e) { return x; } };
            this._l.push(i < 0 ? [dec(part), ''] : [dec(part.slice(0, i)), dec(part.slice(i + 1))]);
        }
    }
    _sync() { if (this._u) { const s = this.toString(); this._u._search = s ? '?' + s : ''; } }
    append(k, v) { this._l.push([String(k), String(v)]); this._sync(); }
    delete(k) { k = String(k); val(this, '_l', this._l.filter((e) => e[0] !== k)); this._sync(); }
    get(k) { const e = this._l.find((x) => x[0] === String(k)); return e ? e[1] : null; }
    getAll(k) { return this._l.filter((x) => x[0] === String(k)).map((x) => x[1]); }
    has(k) { return this._l.some((x) => x[0] === String(k)); }
    set(k, v) {
        k = String(k);
        const i = this._l.findIndex((x) => x[0] === k);
        if (i < 0) this._l.push([k, String(v)]);
        else { this._l[i][1] = String(v); val(this, '_l', this._l.filter((x, j) => j <= i || x[0] !== k)); }
        this._sync();
    }
    sort() { this._l.sort((a, b) => (a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0)); this._sync(); }
    forEach(cb, t) { for (const [k, v] of this._l) cb.call(t, v, k, this); }
    keys() { return this._l.map((e) => e[0])[Symbol.iterator](); }
    values() { return this._l.map((e) => e[1])[Symbol.iterator](); }
    entries() { return this._l.map((e) => [e[0], e[1]])[Symbol.iterator](); }
    [Symbol.iterator]() { return this.entries(); }
    get size() { return this._l.length; }
    toString() {
        const enc = (s) => encodeURIComponent(s).replace(/%20/g, '+').replace(/[!'()~]/g, (c) => '%' + c.charCodeAt(0).toString(16).toUpperCase());
        return this._l.map(([k, v]) => enc(k) + '=' + enc(v)).join('&');
    }
}
class URL {
    constructor(url, base) {
        let s = String(url).trim();
        if (base !== undefined && base !== null) {
            const b = base instanceof URL ? base.href : String(base);
            if (!URL_RE.test(b)) throw new TypeError("Failed to construct 'URL': Invalid base URL");
            const r = __resolve(b, s);
            if (r === null) throw new TypeError("Failed to construct 'URL': Invalid URL");
            s = r;
        }
        const m = URL_RE.exec(s);
        if (!m) throw new TypeError("Failed to construct 'URL': Invalid URL '" + s + "'");
        val(this, '_protocol', m[1].toLowerCase());
        val(this, '_username', m[2] || '');
        val(this, '_password', m[3] || '');
        val(this, '_hostname', (m[4] || '').toLowerCase());
        val(this, '_port', m[5] && m[5] !== DEFAULT_PORT[m[1].toLowerCase()] ? m[5] : '');
        val(this, '_pathname', m[6] || (m[4] !== undefined && this._protocol !== 'data:' && this._protocol !== 'blob:' && this._protocol !== 'about:' ? '/' : ''));
        val(this, '_search', m[7] && m[7] !== '?' ? m[7] : '');
        val(this, '_hash', m[8] && m[8] !== '#' ? m[8] : '');
        val(this, '_sp', null);
    }
    get protocol() { return this._protocol; } set protocol(v) { this._protocol = String(v).replace(/:?$/, ':'); }
    get username() { return this._username; } set username(v) { this._username = String(v); }
    get password() { return this._password; } set password(v) { this._password = String(v); }
    get hostname() { return this._hostname; } set hostname(v) { this._hostname = String(v); }
    get port() { return this._port; } set port(v) { this._port = String(v); }
    get host() { return this._hostname + (this._port ? ':' + this._port : ''); }
    set host(v) { const [h, p] = String(v).split(':'); this._hostname = h; this._port = p || ''; }
    get origin() { return /^(https?|wss?|ftp):$/.test(this._protocol) ? this._protocol + '//' + this.host : 'null'; }
    get pathname() { return this._pathname; } set pathname(v) { v = String(v); this._pathname = v[0] === '/' ? v : '/' + v; }
    get search() { return this._search; }
    set search(v) { v = String(v); this._search = v && v !== '?' ? (v[0] === '?' ? v : '?' + v) : ''; if (this._sp) { const n = new URLSearchParams(this._search); this._sp._l.length = 0; this._sp._l.push(...n._l); } }
    get searchParams() {
        if (!this._sp) { const sp = new URLSearchParams(this._search); sp._u = this; val(this, '_sp', sp); }
        return this._sp;
    }
    get hash() { return this._hash; } set hash(v) { v = String(v); this._hash = v && v !== '#' ? (v[0] === '#' ? v : '#' + v) : ''; }
    get href() {
        const auth = this._username ? this._username + (this._password ? ':' + this._password : '') + '@' : '';
        const slashes = this._hostname || /^(https?|wss?|ftp|file):$/.test(this._protocol) ? '//' : '';
        return this._protocol + slashes + auth + this.host + this._pathname + this._search + this._hash;
    }
    set href(v) { const u = new URL(v); for (const k of ['_protocol', '_username', '_password', '_hostname', '_port', '_pathname', '_search', '_hash']) this[k] = u[k]; this._sp = null; }
    toString() { return this.href; }
    toJSON() { return this.href; }
    static canParse(u, b) { try { new URL(u, b); return true; } catch (e) { return false; } }
    static createObjectURL(blob) { const id = 'blob:' + G.location.origin + '/' + crypto.randomUUID(); BLOBS.set(id, blob); return id; }
    static revokeObjectURL(id) { BLOBS.delete(id); }
}
const BLOBS = new Map();
G.URL = URL;
G.URLSearchParams = URLSearchParams;
G.webkitURL = URL;

/* ---- location and history ----------------------------------------------- */
const loc = {};
for (const k of ['protocol', 'host', 'hostname', 'port', 'pathname', 'search', 'hash', 'origin']) {
    def(loc, k, () => new URL(__getUrl())[k], (v) => {
        const u = new URL(__getUrl());
        u[k] = v;
        if (k === 'hash') { const old = __getUrl(); __setUrl(u.href); G.dispatchEvent(new HashChangeEvent('hashchange', { oldURL: old, newURL: u.href })); }
        else __navigate(u.href, false);
    });
}
def(loc, 'href', () => __getUrl(), (v) => {
    const target = __resolve(__getUrl(), String(v)) || String(v);
    const cur = new URL(__getUrl()), nu = new URL(target);
    if (cur.href.split('#')[0] === nu.href.split('#')[0] && nu.hash) {
        __setUrl(nu.href);
        G.dispatchEvent(new HashChangeEvent('hashchange', { oldURL: cur.href, newURL: nu.href }));
    } else {
        __navigate(target, false);
    }
});
loc.assign = (u) => { loc.href = u; };
loc.replace = (u) => { __navigate(String(u), true); };
loc.reload = () => { __navigate(__getUrl(), true); };
loc.toString = () => __getUrl();
loc.valueOf = () => loc;
loc.ancestorOrigins = [];
def(G, 'location', () => loc, (v) => { loc.href = v; });

let histState = null, histLen = 1;
G.history = {
    get length() { return histLen; },
    get state() { return histState; },
    scrollRestoration: 'auto',
    pushState(state, title, url) { histState = state === undefined ? null : state; histLen++; if (url !== undefined && url !== null) __setUrl(String(url)); },
    replaceState(state, title, url) { histState = state === undefined ? null : state; if (url !== undefined && url !== null) __setUrl(String(url)); },
    back() { __historyGo(-1); }, forward() { __historyGo(1); }, go(d) { __historyGo(d | 0); }
};

/* ---- navigator ---------------------------------------------------------- */
G.navigator = {
    userAgent: 'Mozilla/5.0 (ICDA; x86_64) Surfer/0.1',
    appVersion: '5.0 (ICDA)', appName: 'Netscape', appCodeName: 'Mozilla', product: 'Gecko', productSub: '20030107',
    vendor: '', vendorSub: '', platform: 'ICDA x86_64', language: 'en-US', languages: ['en-US', 'en'],
    onLine: true, cookieEnabled: true, doNotTrack: '1', hardwareConcurrency: 2, maxTouchPoints: 0, deviceMemory: 4,
    webdriver: false, pdfViewerEnabled: false, plugins: [], mimeTypes: [],
    javaEnabled: () => false,
    sendBeacon: () => true,
    vibrate: () => false,
    clipboard: { writeText: () => Promise.resolve(), readText: () => Promise.resolve(''), write: () => Promise.resolve(), read: () => Promise.resolve([]) },
    permissions: { query: () => Promise.resolve({ state: 'prompt', addEventListener() {}, onchange: null }) },
    storage: { estimate: () => Promise.resolve({ quota: 1e9, usage: 0 }), persist: () => Promise.resolve(false), persisted: () => Promise.resolve(false) },
    connection: { effectiveType: '4g', downlink: 10, rtt: 50, saveData: false, addEventListener() {} },
    userActivation: { hasBeenActive: true, isActive: true },
    locks: { request: (n, o, cb) => Promise.resolve((cb || o)({ name: n })) }
};

/* ---- encoding ----------------------------------------------------------- */
function toArrayBuffer(x) {
    if (x instanceof ArrayBuffer) return x;
    if (ArrayBuffer.isView(x)) return x.buffer.slice(x.byteOffset, x.byteOffset + x.byteLength);
    return __utf8Encode(String(x));
}
G.TextEncoder = class TextEncoder {
    get encoding() { return 'utf-8'; }
    encode(s) { return new Uint8Array(__utf8Encode(s === undefined ? '' : String(s))); }
    encodeInto(s, dest) { const b = this.encode(s); const n = Math.min(b.length, dest.length); dest.set(b.subarray(0, n)); return { read: s.length, written: n }; }
};
G.TextDecoder = class TextDecoder {
    constructor(label) { this.encoding = (label || 'utf-8').toLowerCase(); this.fatal = false; this.ignoreBOM = false; }
    decode(buf) {
        if (buf === undefined) return '';
        const ab = toArrayBuffer(buf);
        if (this.encoding === 'latin1' || this.encoding === 'iso-8859-1' || this.encoding === 'ascii') {
            return String.fromCharCode.apply(null, Array.from(new Uint8Array(ab)));
        }
        return __utf8Decode(ab);
    }
};
const B64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
G.btoa = function (s) {
    s = String(s);
    let out = '';
    for (let i = 0; i < s.length; i += 3) {
        const a = s.charCodeAt(i), b = s.charCodeAt(i + 1), c = s.charCodeAt(i + 2);
        if (a > 255 || b > 255 || c > 255) throw new Error('InvalidCharacterError');
        const n = (a << 16) | ((b || 0) << 8) | (c || 0);
        out += B64[n >> 18] + B64[(n >> 12) & 63] + (i + 1 < s.length ? B64[(n >> 6) & 63] : '=') + (i + 2 < s.length ? B64[n & 63] : '=');
    }
    return out;
};
G.atob = function (s) {
    s = String(s).replace(/[\s=]/g, '');
    let out = '', bits = 0, n = 0;
    for (const ch of s) {
        const v = B64.indexOf(ch === '-' ? '+' : ch === '_' ? '/' : ch);
        if (v < 0) throw new Error('InvalidCharacterError');
        n = (n << 6) | v; bits += 6;
        if (bits >= 8) { bits -= 8; out += String.fromCharCode((n >> bits) & 255); }
    }
    return out;
};

/* ---- crypto ------------------------------------------------------------- */
G.crypto = {
    getRandomValues(arr) {
        const bytes = new Uint8Array(__randomBytes(arr.byteLength));
        new Uint8Array(arr.buffer, arr.byteOffset, arr.byteLength).set(bytes);
        return arr;
    },
    randomUUID() {
        const b = new Uint8Array(__randomBytes(16));
        b[6] = (b[6] & 0x0f) | 0x40; b[8] = (b[8] & 0x3f) | 0x80;
        const h = Array.from(b, (x) => x.toString(16).padStart(2, '0')).join('');
        return h.slice(0, 8) + '-' + h.slice(8, 12) + '-' + h.slice(12, 16) + '-' + h.slice(16, 20) + '-' + h.slice(20);
    },
    subtle: {}
};
G.structuredClone = (v) => (v === undefined ? undefined : JSON.parse(JSON.stringify(v)));

/* ---- Blob / FormData / Headers / Response / fetch ----------------------- */
class Blob {
    constructor(parts, opts) {
        const chunks = (parts || []).map((p) => (p instanceof Blob ? new Uint8Array(p._buf) : new Uint8Array(toArrayBuffer(p))));
        const total = chunks.reduce((n, c) => n + c.length, 0);
        const all = new Uint8Array(total);
        let o = 0;
        for (const c of chunks) { all.set(c, o); o += c.length; }
        val(this, '_buf', all.buffer);
        this.type = (opts && opts.type) || '';
    }
    get size() { return this._buf.byteLength; }
    text() { return Promise.resolve(__utf8Decode(this._buf)); }
    arrayBuffer() { return Promise.resolve(this._buf.slice(0)); }
    bytes() { return Promise.resolve(new Uint8Array(this._buf.slice(0))); }
    slice(a, b, type) { const x = new Blob([]); x._buf = this._buf.slice(a, b); x.type = type || this.type; return x; }
    stream() { return null; }
}
class File extends Blob { constructor(parts, name, opts) { super(parts, opts); this.name = String(name); this.lastModified = Date.now(); } }
G.Blob = Blob; G.File = File;
G.FileReader = class FileReader {
    constructor() { this.readyState = 0; this.result = null; this.onload = null; this.onloadend = null; this.onerror = null; }
    _done(r) { this.result = r; this.readyState = 2; const ev = new ProgressEvent('load'); setTimeout(() => { if (this.onload) this.onload(ev); if (this.onloadend) this.onloadend(ev); }, 0); }
    readAsText(b) { b.text().then((t) => this._done(t)); }
    readAsArrayBuffer(b) { b.arrayBuffer().then((t) => this._done(t)); }
    readAsDataURL(b) { b.arrayBuffer().then((t) => this._done('data:' + (b.type || 'application/octet-stream') + ';base64,' + btoa(String.fromCharCode.apply(null, Array.from(new Uint8Array(t)))))); }
    abort() {}
    addEventListener(t, cb) { this['on' + t] = cb; }
};
class FormData {
    constructor(form) {
        val(this, '_l', []);
        if (form && form.elements) {
            for (const e of form.elements) {
                const name = e.getAttribute('name');
                if (!name || e.disabled) continue;
                const type = (e.getAttribute('type') || '').toLowerCase();
                if ((type === 'checkbox' || type === 'radio') && !e.checked) continue;
                if (type === 'submit' || type === 'button' || type === 'reset' || type === 'image' || e.localName === 'button') continue;
                this._l.push([name, e.value]);
            }
        }
    }
    append(k, v) { this._l.push([String(k), v instanceof Blob ? v : String(v)]); }
    delete(k) { val(this, '_l', this._l.filter((e) => e[0] !== String(k))); }
    get(k) { const e = this._l.find((x) => x[0] === String(k)); return e ? e[1] : null; }
    getAll(k) { return this._l.filter((x) => x[0] === String(k)).map((x) => x[1]); }
    has(k) { return this._l.some((x) => x[0] === String(k)); }
    set(k, v) { this.delete(k); this.append(k, v); }
    forEach(cb, t) { for (const [k, v] of this._l) cb.call(t, v, k, this); }
    entries() { return this._l.slice()[Symbol.iterator](); }
    keys() { return this._l.map((e) => e[0])[Symbol.iterator](); }
    values() { return this._l.map((e) => e[1])[Symbol.iterator](); }
    [Symbol.iterator]() { return this.entries(); }
}
G.FormData = FormData;
class Headers {
    constructor(init) {
        val(this, '_m', new Map());
        if (!init) return;
        if (init instanceof Headers) init.forEach((v, k) => this.append(k, v));
        else if (Array.isArray(init)) for (const [k, v] of init) this.append(k, v);
        else for (const k of Object.keys(init)) this.append(k, init[k]);
    }
    append(k, v) { k = String(k).toLowerCase(); const o = this._m.get(k); this._m.set(k, o !== undefined ? o + ', ' + v : String(v)); }
    set(k, v) { this._m.set(String(k).toLowerCase(), String(v)); }
    get(k) { const v = this._m.get(String(k).toLowerCase()); return v === undefined ? null : v; }
    has(k) { return this._m.has(String(k).toLowerCase()); }
    delete(k) { this._m.delete(String(k).toLowerCase()); }
    forEach(cb, t) { for (const [k, v] of this._m) cb.call(t, v, k, this); }
    entries() { return this._m.entries(); }
    keys() { return this._m.keys(); }
    values() { return this._m.values(); }
    getSetCookie() { return []; }
    [Symbol.iterator]() { return this._m.entries(); }
}
G.Headers = Headers;
function parseHeaders(raw) {
    const h = new Headers();
    for (const line of String(raw).split(/\r?\n/)) {
        const i = line.indexOf(':');
        if (i > 0) h.append(line.slice(0, i).trim(), line.slice(i + 1).trim());
    }
    return h;
}
const STATUS_TEXT = { 200: 'OK', 201: 'Created', 204: 'No Content', 301: 'Moved Permanently', 302: 'Found', 304: 'Not Modified',
    400: 'Bad Request', 401: 'Unauthorized', 403: 'Forbidden', 404: 'Not Found', 429: 'Too Many Requests', 500: 'Internal Server Error', 503: 'Service Unavailable' };
/* ---- streams: enough of ReadableStream for fetch bodies and the libraries
 * that build their own (start / pull / cancel; one reader) --------------- */
class ReadableStream {
    constructor(source) {
        source = source || {};
        val(this, '_src', source); val(this, '_q', []); val(this, '_wait', []);
        val(this, '_closed', false); val(this, '_err', null); val(this, '_pulling', false);
        val(this, '_onClose', []);
        this.locked = false;
        const s = this;
        val(this, '_ctl', {
            get desiredSize() { return s._q.length ? 0 : 1; },
            enqueue(c) { if (s._closed) return; if (s._wait.length) s._wait.shift().resolve({ value: c, done: false }); else s._q.push(c); },
            close() { s._close(); },
            error(e) { s._fail(e); }
        });
        try {
            const r = source.start && source.start(this._ctl);
            if (r && typeof r.then === 'function') r.then(null, (e) => this._fail(e));
        } catch (e) { this._fail(e); }
    }
    _close() {
        if (this._closed) return;
        this._closed = true;
        if (!this._q.length) while (this._wait.length) this._wait.shift().resolve({ value: undefined, done: true });
        for (const f of this._onClose.splice(0)) f.resolve();
    }
    _fail(e) {
        this._err = e || new TypeError('The stream is errored.');
        while (this._wait.length) this._wait.shift().reject(this._err);
        for (const f of this._onClose.splice(0)) f.reject(this._err);
    }
    _pull() {
        if (!this._src.pull || this._closed || this._err || this._pulling) return;
        this._pulling = true;
        Promise.resolve().then(() => this._src.pull(this._ctl)).then(() => {
            this._pulling = false;
            if (this._wait.length && !this._q.length) this._pull();
        }, (e) => { this._pulling = false; this._fail(e); });
    }
    _read() {
        if (this._q.length) {
            const value = this._q.shift();
            if (this._closed && !this._q.length) Promise.resolve().then(() => { while (this._wait.length) this._wait.shift().resolve({ value: undefined, done: true }); });
            return Promise.resolve({ value, done: false });
        }
        if (this._err) return Promise.reject(this._err);
        if (this._closed) return Promise.resolve({ value: undefined, done: true });
        return new Promise((resolve, reject) => { this._wait.push({ resolve, reject }); this._pull(); });
    }
    getReader() {
        if (this.locked) throw new TypeError('ReadableStreamDefaultReader constructor can only accept readable streams that are not yet locked to a reader');
        this.locked = true;
        const s = this;
        const closed = new Promise((resolve, reject) => {
            if (s._err) reject(s._err); else if (s._closed && !s._q.length) resolve(); else s._onClose.push({ resolve, reject });
        });
        closed.catch(() => {});
        return {
            closed,
            read: () => s._read(),
            releaseLock: () => { s.locked = false; },
            cancel: (r) => s._cancel(r)
        };
    }
    _cancel(r) {
        this._q.length = 0;
        this._close();
        try { if (this._src.cancel) this._src.cancel(r); } catch (e) {}
        return Promise.resolve();
    }
    cancel(r) { return this.locked ? Promise.reject(new TypeError('Cannot cancel a locked stream')) : this._cancel(r); }
    tee() {
        const r = this.getReader(), outs = [], ctls = [];
        const pump = () => r.read().then(({ value, done }) => {
            if (done) { for (const c of ctls) c.close(); return; }
            for (const c of ctls) c.enqueue(value);
            return pump();
        }, (e) => { for (const c of ctls) c.error(e); });
        for (let i = 0; i < 2; i++) outs.push(new ReadableStream({ start(c) { ctls.push(c); } }));
        pump();
        return outs;
    }
    pipeTo(dest) {
        const r = this.getReader(), w = dest.getWriter();
        const step = () => r.read().then(({ value, done }) => done ? w.close() : Promise.resolve(w.write(value)).then(step));
        return step();
    }
    pipeThrough(t) { this.pipeTo(t.writable); return t.readable; }
    async *[Symbol.asyncIterator]() {
        const r = this.getReader();
        try {
            for (;;) {
                const { value, done } = await r.read();
                if (done) return;
                yield value;
            }
        } finally { r.releaseLock(); }
    }
    values() { return this[Symbol.asyncIterator](); }
    static from(it) {
        return new ReadableStream({
            async start(c) { for await (const x of it) c.enqueue(x); c.close(); }
        });
    }
}
G.ReadableStream = ReadableStream;
class WritableStream {
    constructor(sink) {
        sink = sink || {};
        val(this, '_sink', sink);
        this.locked = false;
        if (sink.start) sink.start({ error() {} });
    }
    getWriter() {
        const s = this._sink;
        this.locked = true;
        let chain = Promise.resolve();
        return {
            ready: Promise.resolve(),
            closed: new Promise(() => {}),
            desiredSize: 1,
            write: (c) => (chain = chain.then(() => s.write && s.write(c, { error() {} }))),
            close: () => (chain = chain.then(() => s.close && s.close())),
            abort: (r) => Promise.resolve(s.abort && s.abort(r)),
            releaseLock: () => { this.locked = false; }
        };
    }
}
G.WritableStream = WritableStream;
class TransformStream {
    constructor(t) {
        t = t || {};
        let ctl;
        this.readable = new ReadableStream({ start(c) { ctl = c; } });
        const tc = { enqueue: (x) => ctl.enqueue(x), error: (e) => ctl.error(e), terminate: () => ctl.close(), desiredSize: 1 };
        if (t.start) t.start(tc);
        this.writable = new WritableStream({
            write: (chunk) => (t.transform ? t.transform(chunk, tc) : tc.enqueue(chunk)),
            close: () => Promise.resolve(t.flush && t.flush(tc)).then(() => ctl.close())
        });
    }
}
G.TransformStream = TransformStream;
G.TextDecoderStream = class TextDecoderStream extends TransformStream {
    constructor(label) {
        const d = new TextDecoder(label);
        super({ transform(chunk, c) { const s = d.decode(chunk, { stream: true }); if (s) c.enqueue(s); } });
    }
};
G.TextEncoderStream = class TextEncoderStream extends TransformStream {
    constructor() {
        const e = new TextEncoder();
        super({ transform(chunk, c) { c.enqueue(e.encode(chunk)); } });
    }
};

class Response {
    constructor(body, init) {
        init = init || {};
        const stream = body instanceof ReadableStream ? body : null;
        val(this, '_stream', stream);
        val(this, '_null', body === undefined || body === null);
        val(this, '_buf', stream || body === undefined || body === null ? new ArrayBuffer(0) : body instanceof Blob ? body._buf : toArrayBuffer(body));
        this.status = init.status === undefined ? 200 : init.status;
        this.statusText = init.statusText !== undefined ? init.statusText : STATUS_TEXT[this.status] || '';
        this.headers = init.headers instanceof Headers ? init.headers : new Headers(init.headers);
        this.url = init.url || '';
        this.redirected = !!init.redirected;
        this.type = 'basic';
        this.bodyUsed = false;
    }
    get ok() { return this.status >= 200 && this.status < 300; }
    /* the body as a stream of one chunk (it has all arrived by now) */
    get body() {
        if (!this._stream) {
            if (this._null) return null;
            const buf = this._buf;
            val(this, '_stream', new ReadableStream({ start(c) { if (buf.byteLength) c.enqueue(new Uint8Array(buf)); c.close(); } }));
        }
        return this._stream;
    }
    _all() {
        this.bodyUsed = true;
        if (!this._stream) return Promise.resolve(this._buf.slice(0));
        const r = this._stream.getReader(), parts = [];
        const step = () => r.read().then(({ value, done }) => {
            if (!done) { parts.push(typeof value === 'string' ? new Uint8Array(__utf8Encode(value)) : new Uint8Array(toArrayBuffer(value))); return step(); }
            let n = 0;
            for (const p of parts) n += p.length;
            const out = new Uint8Array(n);
            n = 0;
            for (const p of parts) { out.set(p, n); n += p.length; }
            return out.buffer;
        });
        return step();
    }
    arrayBuffer() { return this._all(); }
    bytes() { return this._all().then((b) => new Uint8Array(b)); }
    text() { return this._all().then((b) => __utf8Decode(b)); }
    json() { return this.text().then((t) => JSON.parse(t)); }
    blob() { return this._all().then((b) => new Blob([b], { type: this.headers.get('content-type') || '' })); }
    formData() { return this.text().then((t) => { const f = new FormData(); new URLSearchParams(t).forEach((v, k) => f.append(k, v)); return f; }); }
    clone() {
        const init = { status: this.status, statusText: this.statusText, headers: new Headers(this.headers), url: this.url };
        if (this._stream) { const [a, b] = this._stream.tee(); val(this, '_stream', a); return new Response(b, init); }
        return new Response(this._null ? null : this._buf.slice(0), init);
    }
    static json(data, init) { const h = new Headers((init && init.headers) || {}); h.set('content-type', 'application/json'); return new Response(JSON.stringify(data), Object.assign({}, init, { headers: h })); }
    static error() { return new Response(null, { status: 0 }); }
    static redirect(url, status) { return new Response(null, { status: status || 302, headers: { location: url } }); }
}
G.Response = Response;
class Request {
    constructor(input, init) {
        init = init || {};
        this.url = input instanceof Request ? input.url : new URL(String(input), doc.baseURI).href;
        this.method = (init.method || (input instanceof Request ? input.method : 'GET')).toUpperCase();
        this.headers = new Headers(init.headers || (input instanceof Request ? input.headers : undefined));
        val(this, '_body', init.body !== undefined ? init.body : input instanceof Request ? input._body : null);
        this.signal = init.signal || null;
        this.credentials = init.credentials || 'same-origin';
        this.mode = init.mode || 'cors';
        this.cache = init.cache || 'default';
        this.redirect = init.redirect || 'follow';
        this.referrer = 'about:client';
    }
    clone() { return new Request(this); }
    text() { return Promise.resolve(this._body === null ? '' : String(this._body)); }
    json() { return this.text().then(JSON.parse); }
}
G.Request = Request;

class AbortSignal {
    constructor() { this.aborted = false; this.reason = undefined; this.onabort = null; }
    throwIfAborted() { if (this.aborted) throw this.reason; }
    static abort(reason) { const c = new AbortController(); c.abort(reason); return c.signal; }
    static timeout(ms) { const c = new AbortController(); setTimeout(() => c.abort(new DOMException('The operation timed out.', 'TimeoutError')), ms); return c.signal; }
    static any(list) { const c = new AbortController(); for (const s of list) { if (s.aborted) { c.abort(s.reason); break; } s.addEventListener('abort', () => c.abort(s.reason)); } return c.signal; }
}
val(AbortSignal.prototype, 'addEventListener', addEventListener);
val(AbortSignal.prototype, 'removeEventListener', removeEventListener);
val(AbortSignal.prototype, 'dispatchEvent', function (ev) { ev.target = this; invoke(this, ev, 2); return !ev.defaultPrevented; });
class AbortController {
    constructor() { this.signal = new AbortSignal(); }
    abort(reason) {
        if (this.signal.aborted) return;
        this.signal.aborted = true;
        this.signal.reason = reason !== undefined ? reason : new DOMException('signal is aborted without reason', 'AbortError');
        this.signal.dispatchEvent(new Event('abort'));
    }
}
G.AbortController = AbortController;
G.AbortSignal = AbortSignal;
class DOMException extends Error {
    constructor(message, name) { super(message || ''); this.name = name || 'Error'; this.code = 0; }
}
G.DOMException = DOMException;

function bodyOf(body, headers) {
    if (body === undefined || body === null) return null;
    if (typeof body === 'string') { if (!headers.has('content-type')) headers.set('content-type', 'text/plain;charset=UTF-8'); return body; }
    if (body instanceof URLSearchParams) { if (!headers.has('content-type')) headers.set('content-type', 'application/x-www-form-urlencoded;charset=UTF-8'); return body.toString(); }
    if (body instanceof FormData) {
        /* multipart is not supported yet: send as urlencoded */
        if (!headers.has('content-type')) headers.set('content-type', 'application/x-www-form-urlencoded;charset=UTF-8');
        const u = new URLSearchParams();
        body.forEach((v, k) => u.append(k, typeof v === 'string' ? v : ''));
        return u.toString();
    }
    if (body instanceof Blob) { if (body.type && !headers.has('content-type')) headers.set('content-type', body.type); return body._buf; }
    return toArrayBuffer(body);
}
function headerString(h) {
    let s = '';
    h.forEach((v, k) => {
        if (/^(host|content-length|connection|cookie|user-agent|accept-encoding)$/.test(k)) return;
        s += k + ': ' + String(v).replace(/[\r\n]/g, ' ') + '\r\n';
    });
    return s;
}
G.fetch = function (input, init) {
    return new Promise((resolve, reject) => {
        const req = input instanceof Request && !init ? input : new Request(input, init);
        if (/^data:/i.test(req.url)) {
            const m = /^data:([^,]*?)(;base64)?,(.*)$/i.exec(req.url);
            if (!m) { reject(new TypeError('Failed to fetch')); return; }
            const text = m[2] ? atob(m[3]) : decodeURIComponent(m[3]);
            const bytes = Uint8Array.from(text, (c) => c.charCodeAt(0));
            resolve(new Response(bytes, { headers: { 'content-type': m[1] || 'text/plain' }, url: req.url }));
            return;
        }
        if (/^blob:/i.test(req.url)) {
            const b = BLOBS.get(req.url);
            if (b) resolve(new Response(b, { url: req.url })); else reject(new TypeError('Failed to fetch'));
            return;
        }
        const signal = req.signal;
        if (signal && signal.aborted) { reject(signal.reason); return; }
        const body = bodyOf(req._body, req.headers);
        let done = false;
        if (signal) signal.addEventListener('abort', () => { if (!done) { done = true; reject(signal.reason); } });
        __fetch(req.method, req.url, headerString(req.headers), body, (status, headers, buf, url, error) => {
            if (done) return;
            done = true;
            if (error) { reject(new TypeError('Failed to fetch: ' + error)); return; }
            resolve(new Response(buf, { status, headers: parseHeaders(headers), url, redirected: url !== req.url }));
        });
    });
};

/* ---- XMLHttpRequest ----------------------------------------------------- */
class XMLHttpRequest {
    constructor() {
        this.readyState = 0; this.status = 0; this.statusText = ''; this.responseText = ''; this.response = '';
        this.responseType = ''; this.responseURL = ''; this.responseXML = null; this.timeout = 0; this.withCredentials = false;
        this.onreadystatechange = null; this.onload = null; this.onerror = null; this.onloadend = null; this.onabort = null;
        this.ontimeout = null; this.onprogress = null; this.onloadstart = null;
        this.upload = { addEventListener() {}, removeEventListener() {}, onprogress: null };
        val(this, '_h', new Headers()); val(this, '_rh', new Headers()); val(this, '_aborted', false);
    }
    open(method, url, async) { this._method = String(method).toUpperCase(); this._url = new URL(String(url), doc.baseURI).href; this._setState(1); }
    setRequestHeader(k, v) { this._h.append(k, v); }
    getResponseHeader(k) { return this._rh.get(k); }
    getAllResponseHeaders() { let s = ''; this._rh.forEach((v, k) => { s += k + ': ' + v + '\r\n'; }); return s; }
    overrideMimeType() {}
    abort() { this._aborted = true; this._fire('abort'); }
    _setState(s) { this.readyState = s; this._fire('readystatechange'); }
    _fire(type) {
        const ev = new ProgressEvent(type, { loaded: this.responseText.length, total: this.responseText.length, lengthComputable: true });
        ev.target = this; ev.currentTarget = this;
        invoke(this, ev, 2);
    }
    send(body) {
        const headers = new Headers(this._h);
        const b = body === undefined || body === null || this._method === 'GET' || this._method === 'HEAD' ? null : bodyOf(body, headers);
        this._fire('loadstart');
        __fetch(this._method, this._url, headerString(headers), b, (status, raw, buf, url, error) => {
            if (this._aborted) return;
            if (error) { this.status = 0; this._setState(4); this._fire('error'); this._fire('loadend'); return; }
            this.status = status; this.statusText = STATUS_TEXT[status] || ''; this.responseURL = url;
            val(this, '_rh', parseHeaders(raw));
            this._setState(2); this._setState(3);
            const text = __utf8Decode(buf);
            this.responseText = this.responseType === '' || this.responseType === 'text' ? text : '';
            if (this.responseType === 'json') { try { this.response = JSON.parse(text); } catch (e) { this.response = null; } }
            else if (this.responseType === 'arraybuffer') this.response = buf;
            else if (this.responseType === 'blob') this.response = new Blob([buf], { type: this._rh.get('content-type') || '' });
            else if (this.responseType === 'document') { const d = doc.createElement('div'); d.innerHTML = text; this.response = this.responseXML = d; }
            else this.response = text;
            this._setState(4);
            this._fire('load');
            this._fire('loadend');
        });
    }
}
XMLHttpRequest.UNSENT = 0; XMLHttpRequest.OPENED = 1; XMLHttpRequest.HEADERS_RECEIVED = 2; XMLHttpRequest.LOADING = 3; XMLHttpRequest.DONE = 4;
val(XMLHttpRequest.prototype, 'addEventListener', addEventListener);
val(XMLHttpRequest.prototype, 'removeEventListener', removeEventListener);
G.XMLHttpRequest = XMLHttpRequest;

/* ---- storage ------------------------------------------------------------ */
class Storage {
    constructor(persist) {
        val(this, '_persist', persist);
        let data = {};
        if (persist) { try { data = JSON.parse(__storageLoad(G.location.origin) || '{}') || {}; } catch (e) { data = {}; } }
        val(this, '_d', new Map(Object.entries(data)));
    }
    _save() { if (this._persist) __storageSave(G.location.origin, JSON.stringify(Object.fromEntries(this._d))); }
    get length() { return this._d.size; }
    key(i) { return Array.from(this._d.keys())[i] || null; }
    getItem(k) { const v = this._d.get(String(k)); return v === undefined ? null : v; }
    setItem(k, v) { this._d.set(String(k), String(v)); this._save(); }
    removeItem(k) { this._d.delete(String(k)); this._save(); }
    clear() { this._d.clear(); this._save(); }
}
function storageProxy(s) {
    return new Proxy(s, {
        get(t, k) { if (k in t) return typeof t[k] === 'function' ? t[k].bind(t) : t[k]; return typeof k === 'string' ? (t.getItem(k) === null ? undefined : t.getItem(k)) : undefined; },
        set(t, k, v) { t.setItem(k, v); return true; },
        deleteProperty(t, k) { t.removeItem(k); return true; },
        ownKeys(t) { return Array.from(t._d.keys()); },
        has(t, k) { return k in t || t._d.has(String(k)); },
        getOwnPropertyDescriptor(t, k) { return t._d.has(String(k)) ? { value: t._d.get(String(k)), writable: true, enumerable: true, configurable: true } : undefined; }
    });
}
let LOCAL = null, SESSION = null;
def(G, 'localStorage', () => LOCAL || (LOCAL = storageProxy(new Storage(true))));
def(G, 'sessionStorage', () => SESSION || (SESSION = storageProxy(new Storage(false))));
G.Storage = Storage;
G.indexedDB = undefined;
G.caches = undefined;

/* ---- observers and misc platform objects -------------------------------- */
class MutationObserver {
    constructor(cb) { this._cb = cb; }
    observe() {}
    disconnect() {}
    takeRecords() { return []; }
}
G.MutationObserver = G.WebKitMutationObserver = MutationObserver;
/* IntersectionObserver against the viewport, from layout.  Targets are
 * checked a few times a second; each change of state is reported, and the
 * first check of a target always is (as in browsers).  A target without a
 * box of its own yet - an <img> before its src is chosen, filling its
 * parent - is measured by its nearest ancestor that has one, so lazy
 * loaders that wait for a non-empty intersection get started. */
const ioAll = new Set();
let ioTimer = 0;
function ioMargin(m) {
    const v = String(m || '0px').trim().split(/\s+/).map((s) => parseFloat(s) || 0);
    return { top: v[0] || 0, right: v[1] !== undefined ? v[1] : v[0] || 0, bottom: v[2] !== undefined ? v[2] : v[0] || 0,
        left: v[3] !== undefined ? v[3] : v[1] !== undefined ? v[1] : v[0] || 0 };
}
function ioBox(el) {
    let r = el.getBoundingClientRect();
    for (let e = el; (r.width <= 0 || r.height <= 0) && e.parentElement; ) {
        e = e.parentElement;
        r = e.getBoundingClientRect();
    }
    return r;
}
function ioCheck() {
    ioTimer = 0;
    let any = false;
    for (const io of Array.from(ioAll)) {
        if (!io._t.size) continue;
        any = true;
        const m = ioMargin(io.rootMargin), vw = G.innerWidth, vh = G.innerHeight;
        const root = { x: -m.left, y: -m.top, left: -m.left, top: -m.top, width: vw + m.left + m.right, height: vh + m.top + m.bottom,
            right: vw + m.right, bottom: vh + m.bottom };
        const entries = [];
        for (const [el, prev] of Array.from(io._t)) {
            if (!el.isConnected) {
                if (prev !== false) io._t.set(el, false);
                continue;
            }
            const own = el.getBoundingClientRect(), r = ioBox(el);
            const x1 = Math.max(r.left, root.left), y1 = Math.max(r.top, root.top);
            const x2 = Math.min(r.left + r.width, root.right), y2 = Math.min(r.top + r.height, root.bottom);
            const w = Math.max(0, x2 - x1), h = Math.max(0, y2 - y1);
            const hit = w > 0 && h > 0;
            if (prev === hit) continue;
            io._t.set(el, hit);
            entries.push(new IntersectionObserverEntry({
                target: el, isIntersecting: hit, isVisible: hit, time: performance.now(), rootBounds: root,
                intersectionRatio: hit ? Math.min(1, (w * h) / Math.max(1, r.width * r.height)) : 0,
                boundingClientRect: own,
                intersectionRect: hit ? { x: x1, y: y1, left: x1, top: y1, width: w, height: h, right: x2, bottom: y2 }
                                      : { x: 0, y: 0, left: 0, top: 0, width: 0, height: 0, right: 0, bottom: 0 }
            }));
        }
        if (entries.length) { try { io._cb(entries, io); } catch (e) { reportError(e); } }
    }
    if (any) ioTimer = setTimeout(ioCheck, 250);
}
const ioWake = () => { if (!ioTimer) ioTimer = setTimeout(ioCheck, 0); };
G.addEventListener('scroll', () => { if (ioTimer) { clearTimeout(ioTimer); ioTimer = 0; } ioWake(); });
class IntersectionObserver {
    constructor(cb, opts) {
        this._cb = cb; this._t = new Map();
        this.root = (opts && opts.root) || null;
        this.rootMargin = (opts && opts.rootMargin) || '0px';
        this.thresholds = [].concat(opts && opts.threshold !== undefined ? opts.threshold : 0);
        ioAll.add(this);
    }
    observe(el) { if (el && !this._t.has(el)) { this._t.set(el, undefined); ioWake(); } }
    unobserve(el) { this._t.delete(el); }
    disconnect() { this._t.clear(); }
    takeRecords() { return []; }
}
G.IntersectionObserver = IntersectionObserver;
/* a real entry type: polyfills (YouTube ships one) check for
 * 'intersectionRatio' in IntersectionObserverEntry.prototype and otherwise
 * replace the observer above with their own */
class IntersectionObserverEntry {
    constructor(init) { this._e = init || {}; }
}
for (const k of ['target', 'isIntersecting', 'intersectionRatio', 'boundingClientRect', 'intersectionRect', 'rootBounds', 'time', 'isVisible'])
    def(IntersectionObserverEntry.prototype, k, function () { return this._e[k]; });
G.IntersectionObserverEntry = IntersectionObserverEntry;
/* ResizeObserver: observed boxes are measured after layout (every 250 ms
 * while anything is observed); a change, and the first measurement, call
 * back with the content and border box sizes. */
class DOMRectReadOnly {
    constructor(x, y, width, height) { this.x = +x || 0; this.y = +y || 0; this.width = +width || 0; this.height = +height || 0; }
    get left() { return Math.min(this.x, this.x + this.width); }
    get right() { return Math.max(this.x, this.x + this.width); }
    get top() { return Math.min(this.y, this.y + this.height); }
    get bottom() { return Math.max(this.y, this.y + this.height); }
    toJSON() { return { x: this.x, y: this.y, width: this.width, height: this.height, top: this.top, right: this.right, bottom: this.bottom, left: this.left }; }
    static fromRect(r) { r = r || {}; return new this(r.x, r.y, r.width, r.height); }
}
class DOMRect extends DOMRectReadOnly {}
G.DOMRectReadOnly = DOMRectReadOnly;
G.DOMRect = DOMRect;
const roAll = new Set();
let roTimer = 0;
class ResizeObserverEntry {
    constructor(target) {
        const r = target.getBoundingClientRect(), cs = getComputedStyle(target);
        const px = (v) => parseFloat(v) || 0;
        const padX = px(cs.paddingLeft) + px(cs.paddingRight) + px(cs.borderLeftWidth) + px(cs.borderRightWidth);
        const padY = px(cs.paddingTop) + px(cs.paddingBottom) + px(cs.borderTopWidth) + px(cs.borderBottomWidth);
        const cw = Math.max(0, r.width - padX), ch = Math.max(0, r.height - padY);
        this.target = target;
        this.contentRect = new DOMRectReadOnly(px(cs.paddingLeft), px(cs.paddingTop), cw, ch);
        const box = (w, h) => { const a = [{ inlineSize: w, blockSize: h }]; return Object.freeze(a); };
        this.contentBoxSize = box(cw, ch);
        this.borderBoxSize = box(r.width, r.height);
        this.devicePixelContentBoxSize = box(cw, ch);
    }
}
G.ResizeObserverEntry = ResizeObserverEntry;
function roCheck() {
    roTimer = 0;
    for (const ro of Array.from(roAll)) {
        const changed = [];
        for (const [el, last] of ro._t) {
            const e = new ResizeObserverEntry(el);
            const w = e.contentBoxSize[0].inlineSize, h = e.contentBoxSize[0].blockSize;
            if (last && last[0] === w && last[1] === h) continue;
            ro._t.set(el, [w, h]);
            changed.push(e);
        }
        if (changed.length) { try { ro._cb.call(ro, changed, ro); } catch (e) { reportError(e); } }
    }
    if (roAll.size) roTimer = setTimeout(roCheck, 250);
}
class ResizeObserver {
    constructor(cb) {
        if (typeof cb !== 'function') throw new TypeError("Failed to construct 'ResizeObserver': parameter 1 is not of type 'Function'.");
        this._cb = cb;
        this._t = new Map();
    }
    observe(el) {
        if (!this._t.has(el)) this._t.set(el, null);
        roAll.add(this);
        if (roTimer) clearTimeout(roTimer);
        roTimer = setTimeout(roCheck, 0);
    }
    unobserve(el) { this._t.delete(el); if (!this._t.size) roAll.delete(this); }
    disconnect() { this._t.clear(); roAll.delete(this); }
}
G.ResizeObserver = ResizeObserver;
G.PerformanceObserver = class PerformanceObserver { constructor() {} observe() {} disconnect() {} static get supportedEntryTypes() { return []; } };
class MessagePort {
    constructor() { this.onmessage = null; this._other = null; }
    postMessage(data) {
        const o = this._other;
        setTimeout(() => {
            const ev = new MessageEvent('message', { data });
            ev.target = o;
            invoke(o, ev, 2);
        }, 0);
    }
    start() {} close() {}
}
val(MessagePort.prototype, 'addEventListener', addEventListener);
val(MessagePort.prototype, 'removeEventListener', removeEventListener);
G.MessageChannel = class MessageChannel {
    constructor() { this.port1 = new MessagePort(); this.port2 = new MessagePort(); this.port1._other = this.port2; this.port2._other = this.port1; }
};
G.MessagePort = MessagePort;
G.BroadcastChannel = class BroadcastChannel { constructor(n) { this.name = n; this.onmessage = null; } postMessage() {} close() {} addEventListener() {} removeEventListener() {} };
G.Worker = undefined;
G.SharedWorker = undefined;
G.WebSocket = class WebSocket {
    constructor(url) {
        this.url = url; this.readyState = 3; this.onerror = null; this.onclose = null; this.onopen = null; this.onmessage = null;
        setTimeout(() => { if (this.onerror) this.onerror(new Event('error')); if (this.onclose) this.onclose(new Event('close')); }, 0);
    }
    send() {} close() {} addEventListener(t, cb) { this['on' + t] = cb; } removeEventListener() {}
};
G.WebSocket.CONNECTING = 0; G.WebSocket.OPEN = 1; G.WebSocket.CLOSING = 2; G.WebSocket.CLOSED = 3;
G.EventSource = class EventSource { constructor(url) { this.url = url; this.readyState = 2; } close() {} addEventListener() {} };
G.customElements = {
    _d: new Map(),
    define(name, ctor) { this._d.set(name, ctor); },
    get(name) { return this._d.get(name); },
    whenDefined(name) { return Promise.resolve(this._d.get(name)); },
    upgrade() {}, getName(ctor) { for (const [n, c] of this._d) if (c === ctor) return n; return null; }
};
G.Image = function Image(w, h) {
    const img = doc.createElement('img');
    if (w !== undefined) img.setAttribute('width', w);
    if (h !== undefined) img.setAttribute('height', h);
    /* Surfer loads page images itself; script-made images just report success */
    let src = '';
    Object.defineProperty(img, 'src', { get() { return src; }, set(v) { src = __resolve(doc.baseURI, String(v)) || String(v); img.setAttribute('src', v); setTimeout(() => img.dispatchEvent(new Event('load')), 0); }, configurable: true });
    return img;
};
G.Audio = function Audio() { return doc.createElement('audio'); };
G.Option = function Option(text, value, defSel, sel) {
    const o = doc.createElement('option');
    if (text !== undefined) o.textContent = text;
    if (value !== undefined) o.setAttribute('value', value);
    if (defSel) o.setAttribute('selected', '');
    return o;
};
G.DOMParser = class DOMParser {
    parseFromString(text, type) {
        const root = doc.createElement('html');
        root.innerHTML = String(text);
        const body = root.querySelector('body') || root;
        return { documentElement: root, body, head: root.querySelector('head'), querySelector: (s) => root.querySelector(s),
            querySelectorAll: (s) => root.querySelectorAll(s), getElementById: (id) => root.querySelector('#' + CSS.escape(id)),
            getElementsByTagName: (n) => root.getElementsByTagName(n), title: (root.querySelector('title') || { textContent: '' }).textContent };
    }
};
G.XMLSerializer = class XMLSerializer { serializeToString(n) { return n.outerHTML || n.textContent || ''; } };
G.Notification = class Notification { static requestPermission() { return Promise.resolve('denied'); } };
G.Notification.permission = 'denied';
G.speechSynthesis = undefined;
G.visualViewport = { get width() { return G.innerWidth; }, get height() { return G.innerHeight; }, scale: 1, offsetLeft: 0, offsetTop: 0,
    pageLeft: 0, get pageTop() { return G.scrollY; }, addEventListener() {}, removeEventListener() {} };
G.trustedTypes = undefined;
G.isSecureContext = true;
G.origin = loc.origin;
G.crossOriginIsolated = false;
G.HTMLCollection = G.NodeList = function NodeList() {};
G.Attr = function Attr() {};
G.ShadowRoot = function ShadowRoot() {};
G.HTMLUnknownElement = HTMLElement;
G.CharacterData = CharacterData;
G.Location = function Location() {};
G.History = function History() {};
G.Window = function Window() {};
G.Range = function Range() {};
G.Selection = function Selection() {};
G.StyleSheet = G.CSSStyleSheet = function CSSStyleSheet() { this.cssRules = []; this.insertRule = () => 0; this.deleteRule = () => {}; this.replaceSync = () => {}; this.replace = () => Promise.resolve(this); };
G.FontFace = function FontFace(family) { this.family = family; this.load = () => Promise.resolve(this); this.status = 'loaded'; };
/* ---- Intl (QuickJS has none): English-style formatting, UTC ------------- */
if (!G.Intl) {
    const MONTHS = ['January', 'February', 'March', 'April', 'May', 'June', 'July', 'August', 'September', 'October', 'November', 'December'];
    const DAYS = ['Sunday', 'Monday', 'Tuesday', 'Wednesday', 'Thursday', 'Friday', 'Saturday'];
    const pad2 = (n) => String(n).padStart(2, '0');
    const loc = (l) => (Array.isArray(l) ? l[0] : l) || 'en-US';
    function groupDigits(int, sep) { return int.replace(/\B(?=(\d{3})+(?!\d))/g, sep); }
    class NumberFormat {
        constructor(locales, opts) {
            this._o = Object.assign({ style: 'decimal', minimumFractionDigits: undefined, maximumFractionDigits: undefined, useGrouping: true }, opts || {});
            this._l = loc(locales);
        }
        format(n) {
            const o = this._o;
            n = Number(n);
            if (!isFinite(n)) return isNaN(n) ? 'NaN' : (n < 0 ? '-∞' : '∞');
            let v = o.style === 'percent' ? n * 100 : n;
            let maxF = o.maximumFractionDigits !== undefined ? o.maximumFractionDigits : (o.style === 'currency' ? 2 : o.style === 'percent' ? 0 : 3);
            let minF = o.minimumFractionDigits !== undefined ? o.minimumFractionDigits : (o.style === 'currency' ? 2 : 0);
            if (minF > maxF) maxF = minF;
            if (o.notation === 'compact') {
                const units = [[1e12, 'T'], [1e9, 'B'], [1e6, 'M'], [1e3, 'K']];
                for (const [d, s] of units) if (Math.abs(v) >= d) { const x = v / d; return (Math.abs(x) < 10 ? +x.toFixed(1) : Math.round(x)) + s; }
            }
            if (o.maximumSignificantDigits) v = Number(v.toPrecision(o.maximumSignificantDigits));
            let s = Math.abs(v).toFixed(maxF);
            if (s.includes('.')) { s = s.replace(/0+$/, ''); const fr = (s.split('.')[1] || ''); if (fr.length < minF) s += '0'.repeat(minF - fr.length); s = s.replace(/\.$/, ''); }
            let [int, frac] = s.split('.');
            if (o.minimumIntegerDigits) int = int.padStart(o.minimumIntegerDigits, '0');
            if (o.useGrouping !== false) int = groupDigits(int, ',');
            let out = frac ? int + '.' + frac : int;
            if (o.style === 'currency') {
                const sym = { USD: '$', EUR: '€', GBP: '£', JPY: '¥', AZN: '₼', TRY: '₺', RUB: '₽', INR: '₹', CNY: 'CN¥' }[o.currency] || (o.currency + ' ');
                out = sym + out;
            } else if (o.style === 'percent') out += '%';
            else if (o.style === 'unit' && o.unit) out += ' ' + o.unit;
            return (v < 0 && Number(s) !== 0 ? '-' : '') + out;
        }
        formatToParts(n) { return [{ type: 'literal', value: this.format(n) }]; }
        resolvedOptions() { return Object.assign({ locale: this._l, numberingSystem: 'latn' }, this._o); }
        static supportedLocalesOf(l) { return [].concat(l || []); }
    }
    class DateTimeFormat {
        constructor(locales, opts) {
            this._o = Object.assign({}, opts || {});
            this._l = loc(locales);
            const o = this._o;
            if (!o.year && !o.month && !o.day && !o.weekday && !o.hour && !o.minute && !o.second && !o.dateStyle && !o.timeStyle) {
                o.year = 'numeric'; o.month = 'numeric'; o.day = 'numeric';
            }
        }
        _parts(d) {
            d = d === undefined ? new Date() : new Date(d);
            const o = this._o, parts = [];
            const lit = (v) => parts.push({ type: 'literal', value: v });
            if (isNaN(d.getTime())) return [{ type: 'literal', value: 'Invalid Date' }];
            const ds = o.dateStyle, ts = o.timeStyle;
            const year = ds ? 'numeric' : o.year, month = ds === 'full' || ds === 'long' ? 'long' : ds === 'medium' ? 'short' : ds === 'short' ? 'numeric' : o.month;
            const day = ds ? 'numeric' : o.day, weekday = ds === 'full' ? 'long' : o.weekday;
            if (weekday) { const w = DAYS[d.getUTCDay()]; parts.push({ type: 'weekday', value: weekday === 'long' ? w : w.slice(0, 3) }); lit(', '); }
            if (month === 'long' || month === 'short') {
                const m = MONTHS[d.getUTCMonth()];
                parts.push({ type: 'month', value: month === 'long' ? m : m.slice(0, 3) });
                if (day) { lit(' '); parts.push({ type: 'day', value: String(d.getUTCDate()) }); }
                if (year) { lit(', '); parts.push({ type: 'year', value: String(d.getUTCFullYear()) }); }
            } else if (month || day || year) {
                const seg = [];
                if (month) seg.push({ type: 'month', value: month === '2-digit' ? pad2(d.getUTCMonth() + 1) : String(d.getUTCMonth() + 1) });
                if (day) seg.push({ type: 'day', value: day === '2-digit' ? pad2(d.getUTCDate()) : String(d.getUTCDate()) });
                if (year) seg.push({ type: 'year', value: year === '2-digit' ? pad2(d.getUTCFullYear() % 100) : String(d.getUTCFullYear()) });
                seg.forEach((p, i) => { if (i) lit('/'); parts.push(p); });
            }
            const hour = ts ? 'numeric' : o.hour, minute = ts ? '2-digit' : o.minute, second = ts === 'medium' || ts === 'long' ? '2-digit' : o.second;
            if (hour || minute) {
                if (parts.length) lit(', ');
                const h24 = o.hour12 === false || o.hourCycle === 'h23';
                let h = d.getUTCHours();
                const pm = h >= 12;
                if (!h24) h = h % 12 || 12;
                if (hour) parts.push({ type: 'hour', value: hour === '2-digit' || h24 ? pad2(h) : String(h) });
                if (minute) { if (hour) lit(':'); parts.push({ type: 'minute', value: pad2(d.getUTCMinutes()) }); }
                if (second) { lit(':'); parts.push({ type: 'second', value: pad2(d.getUTCSeconds()) }); }
                if (!h24 && hour) { lit(' '); parts.push({ type: 'dayPeriod', value: pm ? 'PM' : 'AM' }); }
            }
            return parts;
        }
        format(d) { return this._parts(d).map((p) => p.value).join(''); }
        formatToParts(d) { return this._parts(d); }
        formatRange(a, b) { return this.format(a) + ' – ' + this.format(b); }
        resolvedOptions() { return Object.assign({ locale: this._l, calendar: 'gregory', numberingSystem: 'latn', timeZone: 'UTC' }, this._o); }
        static supportedLocalesOf(l) { return [].concat(l || []); }
    }
    class Collator {
        constructor(l, o) { this._o = o || {}; }
        compare(a, b) {
            a = String(a); b = String(b);
            if (this._o.sensitivity === 'base' || this._o.sensitivity === 'accent') { a = a.toLowerCase(); b = b.toLowerCase(); }
            if (this._o.numeric) { const x = parseFloat(a), y = parseFloat(b); if (!isNaN(x) && !isNaN(y) && x !== y) return x < y ? -1 : 1; }
            return a < b ? -1 : a > b ? 1 : 0;
        }
        resolvedOptions() { return { locale: 'en-US', usage: 'sort', sensitivity: 'variant' }; }
        static supportedLocalesOf(l) { return [].concat(l || []); }
    }
    class PluralRules {
        constructor(l, o) { this._o = o || {}; }
        select(n) {
            n = Number(n);
            if (this._o.type === 'ordinal') { const t = n % 10, h = n % 100; return t === 1 && h !== 11 ? 'one' : t === 2 && h !== 12 ? 'two' : t === 3 && h !== 13 ? 'few' : 'other'; }
            return n === 1 ? 'one' : 'other';
        }
        resolvedOptions() { return { locale: 'en-US', pluralCategories: ['one', 'other'] }; }
        static supportedLocalesOf(l) { return [].concat(l || []); }
    }
    class RelativeTimeFormat {
        constructor(l, o) { this._o = o || {}; }
        format(v, unit) {
            unit = String(unit).replace(/s$/, '');
            v = Number(v);
            if (this._o.numeric === 'auto' && unit === 'day' && Math.abs(v) <= 1) return v === 0 ? 'today' : v > 0 ? 'tomorrow' : 'yesterday';
            const n = Math.abs(v), u = n === 1 ? unit : unit + 's';
            return v < 0 || Object.is(v, -0) ? n + ' ' + u + ' ago' : 'in ' + n + ' ' + u;
        }
        formatToParts(v, u) { return [{ type: 'literal', value: this.format(v, u) }]; }
        resolvedOptions() { return { locale: 'en-US', style: 'long', numeric: this._o.numeric || 'always' }; }
        static supportedLocalesOf(l) { return [].concat(l || []); }
    }
    class ListFormat {
        constructor(l, o) { this._o = o || {}; }
        format(list) {
            const a = Array.from(list, String);
            const word = this._o.type === 'disjunction' ? 'or' : 'and';
            if (a.length < 2) return a.join('');
            if (a.length === 2) return a[0] + ' ' + word + ' ' + a[1];
            return a.slice(0, -1).join(', ') + ', ' + word + ' ' + a[a.length - 1];
        }
        static supportedLocalesOf(l) { return [].concat(l || []); }
    }
    class Segmenter {
        constructor(l, o) { this._g = (o && o.granularity) || 'grapheme'; }
        segment(s) {
            s = String(s);
            const out = [];
            if (this._g === 'word') {
                const re = /\w+|[^\w]/g;
                let m;
                while ((m = re.exec(s))) out.push({ segment: m[0], index: m.index, input: s, isWordLike: /\w/.test(m[0]) });
            } else {
                let i = 0;
                for (const ch of s) { out.push({ segment: ch, index: i, input: s }); i += ch.length; }
            }
            out.containing = (idx) => out.find((x) => idx >= x.index && idx < x.index + x.segment.length);
            return out;
        }
    }
    class DisplayNames {
        constructor(l, o) { this._o = o || {}; }
        of(code) { return String(code); }
    }
    G.Intl = {
        NumberFormat, DateTimeFormat, Collator, PluralRules, RelativeTimeFormat, ListFormat, Segmenter, DisplayNames,
        Locale: class Locale { constructor(tag) { this.baseName = String(tag); this.language = String(tag).split('-')[0]; } toString() { return this.baseName; } },
        getCanonicalLocales: (l) => [].concat(l || []),
        supportedValuesOf: () => []
    };
    Number.prototype.toLocaleString = function (l, o) { return new NumberFormat(l, o).format(this); };
    Date.prototype.toLocaleDateString = function (l, o) { return new DateTimeFormat(l, Object.assign({ year: 'numeric', month: 'numeric', day: 'numeric' }, o)).format(this); };
    Date.prototype.toLocaleTimeString = function (l, o) { return new DateTimeFormat(l, Object.assign({ hour: 'numeric', minute: '2-digit', second: '2-digit' }, o)).format(this); };
    Date.prototype.toLocaleString = function (l, o) { return new DateTimeFormat(l, o || { year: 'numeric', month: 'numeric', day: 'numeric', hour: 'numeric', minute: '2-digit', second: '2-digit' }).format(this); };
    String.prototype.localeCompare = function (b, l, o) { return new Collator(l, o).compare(this, b); };
}

/* document.all is a falsy object in browsers, which JavaScript itself cannot
 * make.  NaN is the nearest stand-in: falsy (old `if (document.all)` IE
 * checks stay off) and never === anything, so tests like polymer-resin's
 * `if (!v && v !== document.all) return v` let undefined through as in a
 * browser - with undefined they sanitized it into "zClosurez". */
Object.defineProperty(DP, 'all', { get: () => NaN, configurable: true });

/* ---- tag-specific properties off Element.prototype ----------------------
 * Browsers put value, src, href, type ... on HTMLInputElement,
 * HTMLImageElement and so on; an unknown or custom element has none of them.
 * Above they are defined once on Element.prototype for simplicity; here they
 * move to the prototype every standard HTML tag inherits from
 * (__stdElementProto, under the per-tag interfaces), so `'text' in
 * document.createElement('x-y')` is false as in a browser.  polymer-resin
 * (YouTube) relies on that to leave bound properties of custom elements
 * alone. */
{
    const STD = G.__stdElementProto;
    const SPECIFIC = ['value', 'checked', 'selectedIndex', 'htmlFor', 'defaultValue', 'method', 'selected',
        'defaultSelected', 'index', 'text', 'options', 'selectedOptions', 'length', 'add', 'form', 'elements', 'submit',
        'requestSubmit', 'reset', 'checkValidity', 'reportValidity', 'setCustomValidity', 'validity',
        'validationMessage', 'willValidate', 'labels', 'select', 'setSelectionRange', 'setRangeText', 'selectionStart',
        'selectionEnd', 'selectionDirection', 'valueAsNumber', 'files', 'indeterminate', 'play', 'pause', 'load',
        'canPlayType', 'paused', 'currentTime', 'duration', 'getContext', 'toDataURL', 'naturalWidth', 'naturalHeight',
        'complete', 'currentSrc', 'decode', 'contentWindow', 'contentDocument', 'content', 'showModal', 'show', 'close',
        'relList', 'name', 'type', 'alt', 'placeholder', 'rel', 'target', 'download', 'accept', 'autocomplete',
        'enctype', 'pattern', 'min', 'max', 'step', 'crossOrigin', 'referrerPolicy', 'loading', 'decoding', 'media',
        'sizes', 'srcset', 'width', 'height', 'label', 'integrity', 'as', 'charset', 'httpEquiv', 'disabled',
        'readOnly', 'required', 'multiple', 'async', 'defer', 'noModule', 'open', 'controls', 'autoplay', 'loop',
        'muted', 'defaultChecked', 'noValidate', 'formNoValidate', 'isMap', 'href', 'src', 'action', 'cite', 'poster',
        'formAction'];
    if (STD) {
        for (const name of SPECIFIC) {
            const d = Object.getOwnPropertyDescriptor(EP, name);
            if (!d) continue;
            Object.defineProperty(STD, name, d);
            delete EP[name];
        }
        delete G.__stdElementProto;
    }
}

/* ---- audio and video: HTMLMediaElement, MediaSource, SourceBuffer ---------
 * Playback is native (media_el.c, reached through __media*).  This layer
 * gives it the web's interfaces and turns the player's state into media
 * events, looked at every 100 ms while a player is open. */
{
    const VP = G.HTMLVideoElement.prototype, AP = G.HTMLAudioElement.prototype;
    const MP = Object.create(Object.getPrototypeOf(VP));
    Object.setPrototypeOf(VP, MP);
    Object.setPrototypeOf(AP, MP);
    const HTMLMediaElement = function HTMLMediaElement() { throw new TypeError('Illegal constructor'); };
    HTMLMediaElement.prototype = MP;
    val(MP, 'constructor', HTMLMediaElement);
    const CONSTS = { NETWORK_EMPTY: 0, NETWORK_IDLE: 1, NETWORK_LOADING: 2, NETWORK_NO_SOURCE: 3, HAVE_NOTHING: 0,
        HAVE_METADATA: 1, HAVE_CURRENT_DATA: 2, HAVE_FUTURE_DATA: 3, HAVE_ENOUGH_DATA: 4 };
    for (const k in CONSTS) { HTMLMediaElement[k] = CONSTS[k]; val(MP, k, CONSTS[k]); }
    G.HTMLMediaElement = HTMLMediaElement;

    class TimeRanges {
        constructor(list) { val(this, '_r', list || []); }
        get length() { return this._r.length >> 1; }
        start(i) { if (!(i >= 0 && i < this.length)) throw new DOMException('Index out of range', 'IndexSizeError'); return this._r[2 * i]; }
        end(i) { if (!(i >= 0 && i < this.length)) throw new DOMException('Index out of range', 'IndexSizeError'); return this._r[2 * i + 1]; }
    }
    G.TimeRanges = TimeRanges;
    class MediaError {
        constructor(code, message) { this.code = code; this.message = message || ''; }
    }
    Object.assign(MediaError, { MEDIA_ERR_ABORTED: 1, MEDIA_ERR_NETWORK: 2, MEDIA_ERR_DECODE: 3, MEDIA_ERR_SRC_NOT_SUPPORTED: 4 });
    G.MediaError = MediaError;

    /* what the decoders take: H.264, VP8 and 8-bit VP9 with AAC, Opus or
     * Vorbis, in MP4 or WebM, up to 1080p */
    function support(type) {
        const m = /^\s*([^;\s]+)\s*(.*)$/.exec(String(type === undefined ? '' : type));
        if (!m) return '';
        const mime = m[1].toLowerCase(), params = {};
        m[2].replace(/;\s*([\w-]+)\s*=\s*("([^"]*)"|[^;]*)/g, (_, k, v, q) => { params[k.toLowerCase()] = (q !== undefined ? q : v).trim(); return ''; });
        const mp4 = /^(video|audio)\/(mp4|x-m4v|x-m4a)$/.test(mime), webm = /^(video|audio)\/webm$/.test(mime);
        if (!mp4 && !webm) return '';
        if ((params.width && +params.width > 1920) || (params.height && +params.height > 1080)) return '';
        if (params.framerate && +params.framerate > 60) return '';
        if (params.eotf && !/^bt709$/i.test(params.eotf)) return '';
        if (params.channels && +params.channels > 2) return '';
        if (!('codecs' in params)) return 'maybe';
        for (let c of params.codecs.split(',')) {
            c = c.trim().toLowerCase();
            if (!c) continue;
            const ok = mp4 ? /^avc[13](\.[0-9a-f]{6})?$/.test(c) || /^mp4a(\.40(\.(2|5|29))?)?$/.test(c)
                           : /^vp0?8(\.0)?$/.test(c) || /^vp9(\.0)?$/.test(c) || /^vp09\.00(\.\d+\.08(\..*)?)?$/.test(c) ||
                             c === 'opus' || c === 'vorbis';
            if (!ok) return '';
        }
        return 'probably';
    }
    G.navigator.mediaCapabilities = {
        decodingInfo(cfg) {
            const v = cfg && cfg.video, a = cfg && cfg.audio;
            let ok = !!(v || a);
            if (v) ok = ok && support(v.contentType) !== '' && (v.width || 0) <= 1920 && (v.height || 0) <= 1080 && (v.framerate || 0) <= 60;
            if (a) ok = ok && support(a.contentType) !== '';
            return Promise.resolve({ supported: ok, smooth: ok && (!v || (v.height || 0) <= 720), powerEfficient: false, keySystemAccess: null, configuration: cfg });
        },
        encodingInfo(cfg) { return Promise.resolve({ supported: false, smooth: false, powerEfficient: false, configuration: cfg }); }
    };

    const MS_URLS = new Map();
    const ST = new WeakMap();
    function st(el) {
        let s = ST.get(el);
        if (!s) {
            s = { h: -1, src: '', ms: null, timer: 0, last: null, ready: 0, paused: true, seeking: false, volume: 1,
                  muted: el.hasAttribute('muted'), rate: 1, ended: false, error: null, dur: NaN, network: 0,
                  lastUpdate: 0, lastProgress: 0, playingFired: false };
            ST.set(el, s);
        }
        return s;
    }
    const fire = (t, type) => t.dispatchEvent(new Event(type));
    const queue = (t, type) => setTimeout(() => fire(t, type), 0);

    function currentURL(el) {
        const a = el.getAttribute('src');
        if (a) return a;
        for (const c of el.children) {
            if (c.localName !== 'source' || !c.getAttribute('src')) continue;
            if (!c.getAttribute('type') || support(c.getAttribute('type'))) return c.getAttribute('src');
        }
        return '';
    }
    function release(s) {
        if (s.timer) { clearInterval(s.timer); s.timer = 0; }
        if (s.ms) { const ms = s.ms; s.ms = null; ms._detach(); }
        if (s.h >= 0) { __mediaCmd(s.h, 'close'); s.h = -1; }
    }
    function load(el) {
        const s = st(el);
        const had = s.h >= 0 || s.ready > 0;
        release(s);
        Object.assign(s, { ready: 0, ended: false, error: null, dur: NaN, seeking: false, playingFired: false, last: null });
        if (had) queue(el, 'emptied');
        const url = currentURL(el);
        s.src = url ? (/^blob:/.test(url) ? url : new URL(url, doc.baseURI).href) : '';
        if (!url) { s.network = 0; s.paused = true; return; }
        s.network = 2;
        const ms = MS_URLS.get(url);
        s.h = __mediaOpen(ms ? null : s.src);
        if (s.h < 0) {
            s.network = 3;
            s.error = new MediaError(4, 'MEDIA_ELEMENT_ERROR: Format error');
            queue(el, 'error');
            return;
        }
        __mediaCmd(s.h, 'bind', el);
        __mediaCmd(s.h, 'volume', s.volume, s.muted ? 1 : 0);
        queue(el, 'loadstart');
        if (ms) { s.ms = ms; ms._attach(el, s.h); }
        s.timer = setInterval(() => poll(el, s), 100);
        if (!s.paused || el.hasAttribute('autoplay')) {
            s.paused = false;
            __mediaCmd(s.h, 'play');
            if (el.hasAttribute('autoplay')) queue(el, 'play');
        }
    }
    function poll(el, s) {
        if (s.h < 0) return;
        const a = __mediaState(s.h);
        if (!a) return;
        const prev = s.last || [0, NaN, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0];
        s.last = a;
        const time = a[0], ready = a[3], ended = a[5], waiting = a[6], error = a[7], now = performance.now();
        if (error && !s.error) {
            s.error = new MediaError(error, error === 4 ? 'MEDIA_ELEMENT_ERROR: Format error' : error === 3 ? 'PIPELINE_ERROR_DECODE' : 'MEDIA_ELEMENT_ERROR: Network error');
            s.network = 3;
            fire(el, 'error');
            return;
        }
        if (!el.isConnected && !s.paused) MP.pause.call(el);
        const dur = s.ms ? s.ms._duration : a[1] > 0 ? a[1] : NaN;
        if (ready >= 1 && s.ready < 1) {
            s.ready = 1;
            s.dur = dur;
            if (!isNaN(dur)) fire(el, 'durationchange');
            if (a[8]) fire(el, 'resize');
            fire(el, 'loadedmetadata');
        } else if (s.ready >= 1 && !(dur === s.dur || (isNaN(dur) && isNaN(s.dur)))) {
            s.dur = dur;
            fire(el, 'durationchange');
        }
        if (s.ready >= 1 && prev[8] && (a[8] !== prev[8] || a[9] !== prev[9])) fire(el, 'resize');
        if (ready >= 2 && s.ready < 2) { s.ready = 2; fire(el, 'loadeddata'); }
        if (ready >= 3 && s.ready < 3) {
            s.ready = 3;
            fire(el, 'canplay');
            if (!s.paused && !waiting) { s.playingFired = true; fire(el, 'playing'); }
        }
        if (ready >= 4 && s.ready < 4) { s.ready = 4; fire(el, 'canplaythrough'); }
        if (ready < s.ready && ready >= 1) s.ready = ready;
        if (s.seeking && ready >= 2) {
            s.seeking = false;
            fire(el, 'timeupdate');
            fire(el, 'seeked');
        }
        if (!s.paused) {
            if (waiting && !prev[6]) { s.playingFired = false; fire(el, 'waiting'); }
            else if (!waiting && !s.playingFired && ready >= 3) { s.playingFired = true; fire(el, 'playing'); }
            if (time !== prev[0] && now - s.lastUpdate >= 250) { s.lastUpdate = now; fire(el, 'timeupdate'); }
        }
        if (a[2] !== prev[2] && now - s.lastProgress >= 350) { s.lastProgress = now; fire(el, 'progress'); }
        if (ended && !s.ended) {
            if (el.hasAttribute('loop')) { __mediaCmd(s.h, 'seek', 0); __mediaCmd(s.h, 'play'); return; }
            s.ended = true;
            s.paused = true;
            fire(el, 'timeupdate');
            fire(el, 'pause');
            fire(el, 'ended');
        }
    }

    val(MP, 'load', function () { load(this); });
    val(MP, 'canPlayType', function (t) { return support(t); });
    val(MP, 'play', function () {
        const s = st(this);
        if (s.h < 0 && !s.error) { s.paused = false; load(this); }
        if (s.h < 0) return Promise.reject(new DOMException('The element has no supported sources.', 'NotSupportedError'));
        if (s.ended) { s.ended = false; s.seeking = true; __mediaCmd(s.h, 'seek', 0); }
        if (s.paused) {
            s.paused = false;
            queue(this, 'play');
            if (s.ready >= 3) { s.playingFired = true; queue(this, 'playing'); }
        }
        __mediaCmd(s.h, 'play');
        return Promise.resolve();
    });
    val(MP, 'pause', function () {
        const s = st(this);
        if (s.h < 0 && !s.src && currentURL(this)) load(this);
        if (!s.paused) {
            s.paused = true;
            s.playingFired = false;
            if (s.h >= 0) __mediaCmd(s.h, 'pause');
            queue(this, 'timeupdate');
            queue(this, 'pause');
        }
    });
    val(MP, 'fastSeek', function (t) { this.currentTime = t; });
    val(MP, 'getVideoPlaybackQuality', function () {
        const a = st(this).last;
        return { creationTime: performance.now(), totalVideoFrames: a ? a[10] + a[11] : 0, droppedVideoFrames: a ? a[11] : 0, corruptedVideoFrames: 0 };
    });
    val(MP, 'addTextTrack', function (kind, label, lang) { return { kind, label: label || '', language: lang || '', mode: 'hidden', cues: [], activeCues: [], addCue() {}, removeCue() {}, addEventListener() {}, removeEventListener() {} }; });
    val(MP, 'setSinkId', function () { return Promise.resolve(); });
    val(MP, 'setMediaKeys', function (k) { return k ? Promise.reject(new DOMException('Not supported', 'NotSupportedError')) : Promise.resolve(); });
    val(MP, 'requestPictureInPicture', function () { return Promise.reject(new DOMException('Not supported', 'NotSupportedError')); });
    def(MP, 'src', function () { const v = this.getAttribute('src'); return v ? (/^blob:/.test(v) ? v : new URL(v, doc.baseURI).href) : ''; },
        function (v) { this.setAttribute('src', String(v)); load(this); });
    def(MP, 'srcObject', function () { return st(this).obj || null; }, function (v) {
        const s = st(this);
        s.obj = v || null;
        if (v instanceof MediaSource) this.setAttribute('src', URL.createObjectURL(v));
        else this.removeAttribute('src');
        load(this);
    });
    def(MP, 'currentSrc', function () { return st(this).src; });
    def(MP, 'currentTime', function () {
        const s = st(this);
        if (s.h < 0) return s.pending || 0;
        const a = __mediaState(s.h);
        return a ? a[0] : 0;
    }, function (v) {
        const s = st(this);
        v = +v;
        if (!isFinite(v)) throw new TypeError("Failed to set the 'currentTime' property: The provided double value is non-finite.");
        if (s.h < 0) { s.pending = v; return; }
        s.ended = false;
        s.seeking = true;
        s.playingFired = false;
        __mediaCmd(s.h, 'seek', Math.max(0, v));
        queue(this, 'seeking');
    });
    def(MP, 'duration', function () { const s = st(this); return s.h < 0 || s.ready < 1 ? NaN : s.dur; });
    def(MP, 'paused', function () { return st(this).paused; });
    def(MP, 'ended', function () { return st(this).ended; });
    def(MP, 'seeking', function () { return st(this).seeking; });
    def(MP, 'readyState', function () { return st(this).ready; });
    def(MP, 'networkState', function () { return st(this).network; });
    def(MP, 'error', function () { return st(this).error; });
    def(MP, 'buffered', function () { const s = st(this); return new TimeRanges(s.h >= 0 ? __mediaBuffered(s.h, -1) : []); });
    def(MP, 'seekable', function () { const s = st(this); return new TimeRanges(s.h >= 0 && s.dur > 0 ? [0, s.dur] : []); });
    def(MP, 'played', function () { return new TimeRanges([]); });
    def(MP, 'volume', function () { return st(this).volume; }, function (v) {
        const s = st(this);
        v = +v;
        if (!(v >= 0 && v <= 1)) throw new DOMException('The volume provided is outside the range [0, 1].', 'IndexSizeError');
        if (v === s.volume) return;
        s.volume = v;
        if (s.h >= 0) __mediaCmd(s.h, 'volume', s.volume, s.muted ? 1 : 0);
        queue(this, 'volumechange');
    });
    def(MP, 'muted', function () { return st(this).muted; }, function (v) {
        const s = st(this);
        v = !!v;
        if (v === s.muted) return;
        s.muted = v;
        if (s.h >= 0) __mediaCmd(s.h, 'volume', s.volume, s.muted ? 1 : 0);
        queue(this, 'volumechange');
    });
    def(MP, 'defaultMuted', function () { return this.hasAttribute('muted'); }, function (v) { if (v) this.setAttribute('muted', ''); else this.removeAttribute('muted'); });
    def(MP, 'playbackRate', function () { return st(this).rate; }, function (v) { const s = st(this); if (+v !== s.rate) { s.rate = +v; queue(this, 'ratechange'); } });
    def(MP, 'defaultPlaybackRate', function () { return 1; }, () => {});
    def(MP, 'preservesPitch', function () { return true; }, () => {});
    def(MP, 'sinkId', function () { return ''; });
    def(MP, 'mediaKeys', function () { return null; });
    def(MP, 'textTracks', function () {
        const s = st(this);
        if (!s.tracks) s.tracks = Object.assign([], { addEventListener() {}, removeEventListener() {}, getTrackById: () => null, onchange: null, onaddtrack: null, onremovetrack: null });
        return s.tracks;
    });
    def(MP, 'disableRemotePlayback', function () { return true; }, () => {});
    def(VP, 'videoWidth', function () { const a = st(this).last; return a && st(this).ready >= 1 ? a[8] : 0; });
    def(VP, 'videoHeight', function () { const a = st(this).last; return a && st(this).ready >= 1 ? a[9] : 0; });
    def(VP, 'webkitDecodedFrameCount', function () { const a = st(this).last; return a ? a[10] + a[11] : 0; });
    def(VP, 'webkitDroppedFrameCount', function () { const a = st(this).last; return a ? a[11] : 0; });
    def(VP, 'disablePictureInPicture', function () { return true; }, () => {});
    G.Audio = function Audio(src) { const a = doc.createElement('audio'); if (src !== undefined) a.src = src; return a; };
    G.Audio.prototype = AP;

    /* ---- Media Source Extensions ---- */
    const invalid = (m) => new DOMException(m, 'InvalidStateError');
    const GONE = 'This SourceBuffer has been removed from the parent media source.';
    const NOT_OPEN = "The MediaSource's readyState is not 'open'.";
    class SourceBufferList extends G.EventTarget {
        constructor() { super(); val(this, '_l', []); this.onaddsourcebuffer = this.onremovesourcebuffer = null; }
        get length() { return this._l.length; }
        _sync() { for (let i = 0; i < 8; i++) { if (i < this._l.length) val(this, i, this._l[i]); else delete this[i]; } }
        [Symbol.iterator]() { return this._l[Symbol.iterator](); }
    }
    G.SourceBufferList = SourceBufferList;
    class SourceBuffer extends G.EventTarget {
        constructor(ms, id, type) {
            super();
            val(this, '_ms', ms); val(this, '_id', id); val(this, '_type', type); val(this, '_tmr', 0);
            this.updating = false;
            this.mode = 'segments';
            this.timestampOffset = 0;
            this.appendWindowStart = 0;
            this.appendWindowEnd = Infinity;
            this.onupdatestart = this.onupdate = this.onupdateend = this.onerror = this.onabort = null;
        }
        get buffered() {
            const ms = this._ms;
            if (!ms || ms._h < 0) throw invalid(GONE);
            return new TimeRanges(__mediaBuffered(ms._h, this._id));
        }
        _begin() {
            const ms = this._ms;
            if (!ms || ms._h < 0) throw invalid(GONE);
            if (this.updating) throw invalid("This SourceBuffer is still processing an 'appendBuffer' or 'remove' operation.");
            if (ms.readyState === 'ended') { ms.readyState = 'open'; ms._fire('sourceopen'); }
            this.updating = true;
        }
        _finish(ok) {
            this._tmr = 0;
            if (!this.updating) return;
            this.updating = false;
            this._fire(ok ? 'update' : 'error');
            this._fire('updateend');
        }
        appendBuffer(data) {
            this._begin();
            const bytes = ArrayBuffer.isView(data) ? data.buffer.slice(data.byteOffset, data.byteOffset + data.byteLength) : data.slice(0);
            this._tmr = setTimeout(() => {
                this._fire('updatestart');
                const ms = this._ms;
                this._finish(!!ms && ms._h >= 0 && __mediaAppend(ms._h, this._id, bytes) === 0);
            }, 0);
        }
        remove(start, end) {
            this._begin();
            this._tmr = setTimeout(() => { this._fire('updatestart'); this._finish(true); }, 0);
        }
        abort() {
            if (!this.updating) return;
            clearTimeout(this._tmr);
            this.updating = false;
            this._fire('abort');
            this._fire('updateend');
        }
        changeType(type) { if (!support(type)) throw new DOMException('Unsupported type', 'NotSupportedError'); this._type = type; }
        _fire(type) { this.dispatchEvent(new Event(type)); }
    }
    G.SourceBuffer = SourceBuffer;
    class MediaSource extends G.EventTarget {
        constructor() {
            super();
            this.readyState = 'closed';
            val(this, '_h', -1); val(this, '_el', null); val(this, '_duration', NaN);
            this.sourceBuffers = new SourceBufferList();
            this.activeSourceBuffers = this.sourceBuffers;
            this.onsourceopen = this.onsourceended = this.onsourceclose = null;
        }
        static isTypeSupported(t) { return support(t) !== ''; }
        get duration() { return this.readyState === 'closed' ? NaN : this._duration; }
        set duration(v) {
            v = +v;
            if (isNaN(v) || v < 0) throw new TypeError('The value provided is not a valid duration.');
            if (this.readyState !== 'open') throw invalid(NOT_OPEN);
            if (this.sourceBuffers._l.some((b) => b.updating)) throw invalid('A SourceBuffer is updating.');
            this._duration = v;
            if (this._h >= 0) __mediaCmd(this._h, 'duration', v);
        }
        addSourceBuffer(type) {
            if (!support(type)) throw new DOMException("The type provided ('" + type + "') is unsupported.", 'NotSupportedError');
            if (this.readyState !== 'open' || this._h < 0) throw invalid(NOT_OPEN);
            const id = __mediaCmd(this._h, 'addSource');
            if (id < 0) throw new DOMException('This MediaSource has reached the limit of SourceBuffer objects it can handle.', 'QuotaExceededError');
            const sb = new SourceBuffer(this, id, type);
            this.sourceBuffers._l.push(sb);
            this.sourceBuffers._sync();
            queue(this.sourceBuffers, 'addsourcebuffer');
            return sb;
        }
        removeSourceBuffer(sb) {
            const l = this.sourceBuffers._l, i = l.indexOf(sb);
            if (i < 0) throw new DOMException('The SourceBuffer provided is not contained in this MediaSource.', 'NotFoundError');
            sb.abort();
            l.splice(i, 1);
            this.sourceBuffers._sync();
            sb._ms = null;
            queue(this.sourceBuffers, 'removesourcebuffer');
        }
        endOfStream(err) {
            if (this.readyState !== 'open') throw invalid(NOT_OPEN);
            this.readyState = 'ended';
            if (!err && this._h >= 0) {
                __mediaCmd(this._h, 'eos');
                if (isNaN(this._duration)) {
                    const r = __mediaBuffered(this._h, -1);
                    if (r.length) { this._duration = r[r.length - 1]; __mediaCmd(this._h, 'duration', this._duration); }
                }
            }
            this._fire('sourceended');
        }
        setLiveSeekableRange() {}
        clearLiveSeekableRange() {}
        _attach(el, h) {
            this._el = el;
            this._h = h;
            this.readyState = 'open';
            queue(this, 'sourceopen');
        }
        _detach() {
            if (this.readyState === 'closed') return;
            this.readyState = 'closed';
            this._h = -1;
            this._el = null;
            this._duration = NaN;
            for (const sb of this.sourceBuffers._l) { sb.abort(); sb._ms = null; }
            this.sourceBuffers._l.length = 0;
            this.sourceBuffers._sync();
            queue(this, 'sourceclose');
        }
        _fire(type) { this.dispatchEvent(new Event(type)); }
    }
    MediaSource.canConstructInDedicatedWorker = false;
    G.MediaSource = MediaSource;
    const createURL = URL.createObjectURL;
    URL.createObjectURL = function (obj) {
        if (obj instanceof MediaSource) {
            const id = 'blob:' + G.location.origin + '/' + crypto.randomUUID();
            MS_URLS.set(id, obj);
            return id;
        }
        return createURL.call(this, obj);
    };
}

/* ---- DOM change notifications: custom elements and MutationObserver ------
 * The tree changes only through a few native primitives (appendChild,
 * insertBefore, removeChild, replaceChild, __remove, the innerHTML family,
 * textContent, setAttribute / removeAttribute, data); the conveniences above
 * all go through them.  They are wrapped once here and report each change to
 * the custom element registry and to MutationObservers.  With no custom
 * element defined and nothing observed the wrappers only test two sizes. */
{
    const NP = Node.prototype, EP2 = Element.prototype;
    const ceDefs = new Map(), ceByCtor = new Map(), ceWaiting = new Map();
    const ceState = new WeakMap();          /* element -> 'custom' | 'pending' | 'failed' */
    const ceUpgrading = [];                 /* construction stack for upgrades */
    const moRegs = new Map();               /* node -> [{ observer, options }] */
    const moPending = new Set();
    let moScheduled = false;
    const watching = () => ceDefs.size > 0 || moRegs.size > 0;
    const nativeCreate = Document.prototype.createElement;

    const ceCall = (el, name, ...args) => {
        const f = el[name];
        if (typeof f === 'function') { try { f.apply(el, args); } catch (e) { reportError(e); } }
    };
    function ceUpgrade(el) {
        if (ceState.has(el)) return;
        const def = ceDefs.get(el.localName);
        if (!def) return;
        ceState.set(el, 'pending');
        ceUpgrading.push(el);
        try {
            new def.ctor();
        } catch (e) {
            ceUpgrading.pop();
            ceState.set(el, 'failed');
            reportError(e);
            return;
        }
        ceUpgrading.pop();
        ceState.set(el, 'custom');
        if (def.attrs.size)
            for (const a of def.attrs) { const v = el.getAttribute(a); if (v !== null) ceCall(el, 'attributeChangedCallback', a, null, v, null); }
        if (el.isConnected) ceCall(el, 'connectedCallback');
    }
    const elementsIn = (n) => {
        if (!n || (n.nodeType !== 1 && n.nodeType !== 11 && n.nodeType !== 9)) return [];
        const list = n.nodeType === 1 ? [n] : [];
        if (n.firstChild) for (const e of n.querySelectorAll('*')) list.push(e);
        return list;
    };
    function ceConnected(nodes) {
        if (!ceDefs.size) return;
        for (const n of nodes) for (const el of elementsIn(n)) {
            const s = ceState.get(el);
            if (s === undefined) { if (el.isConnected) ceUpgrade(el); }
            else if (s === 'custom' && el.isConnected) ceCall(el, 'connectedCallback');
        }
    }
    function ceDisconnected(nodes) {
        if (!ceDefs.size) return;
        for (const n of nodes) for (const el of elementsIn(n)) if (ceState.get(el) === 'custom') ceCall(el, 'disconnectedCallback');
    }

    /* `class X extends HTMLElement`: super() hands back a real element */
    const NativeHTMLElement = G.HTMLElement, HEP = NativeHTMLElement.prototype;
    function HTMLElement() {
        const def = new.target && ceByCtor.get(new.target);
        if (!def) throw new TypeError('Illegal constructor');
        let el = ceUpgrading.length ? ceUpgrading[ceUpgrading.length - 1] : null;
        if (el) ceUpgrading[ceUpgrading.length - 1] = null;
        else el = nativeCreate.call(doc, def.name);
        Object.setPrototypeOf(el, new.target.prototype);
        if (!ceState.has(el) || ceState.get(el) === 'pending') ceState.set(el, 'custom');
        return el;
    }
    HTMLElement.prototype = HEP;
    Object.defineProperty(HEP, 'constructor', { value: HTMLElement, writable: true, configurable: true });
    Object.setPrototypeOf(HTMLElement, Object.getPrototypeOf(NativeHTMLElement));
    G.HTMLElement = HTMLElement;

    G.CustomElementRegistry = function CustomElementRegistry() {};
    G.customElements = Object.create(G.CustomElementRegistry.prototype);
    Object.assign(G.customElements, {
        define(name, ctor, opts) {
            name = String(name);
            if (typeof ctor !== 'function') throw new TypeError('constructor expected');
            if (ceDefs.has(name) || ceByCtor.has(ctor)) throw new DOMException('"' + name + '" has already been defined', 'NotSupportedError');
            if (opts && opts.extends) return;            /* customized built-ins: not supported */
            const def = { name, ctor, attrs: new Set([].concat(ctor.observedAttributes || []).map(String)) };
            ceDefs.set(name, def);
            ceByCtor.set(ctor, def);
            for (const el of Array.from(doc.querySelectorAll(name))) ceUpgrade(el);
            const w = ceWaiting.get(name);
            if (w) { ceWaiting.delete(name); w.resolve(ctor); }
        },
        get(name) { const d = ceDefs.get(String(name)); return d ? d.ctor : undefined; },
        getName(ctor) { const d = ceByCtor.get(ctor); return d ? d.name : null; },
        whenDefined(name) {
            name = String(name);
            const d = ceDefs.get(name);
            if (d) return Promise.resolve(d.ctor);
            let w = ceWaiting.get(name);
            if (!w) { w = {}; w.promise = new Promise((r) => { w.resolve = r; }); ceWaiting.set(name, w); }
            return w.promise;
        },
        upgrade(root) { for (const el of elementsIn(root)) ceUpgrade(el); }
    });
    Object.defineProperty(Document.prototype, 'createElement', {
        value: function (name, opts) {
            const d = ceDefs.get(String(name).toLowerCase());
            if (d && this === doc) { try { return new d.ctor(); } catch (e) { reportError(e); } }
            return nativeCreate.apply(this, arguments);
        }, writable: true, configurable: true
    });

    /* MutationObserver */
    class MutationRecord {
        constructor(type, target, f) {
            this.type = type; this.target = target;
            this.addedNodes = f.added || []; this.removedNodes = f.removed || [];
            this.previousSibling = f.prev || null; this.nextSibling = f.next || null;
            this.attributeName = f.name || null; this.attributeNamespace = null;
            this.oldValue = f.old === undefined ? null : f.old;
        }
    }
    class MutationObserver {
        constructor(cb) {
            if (typeof cb !== 'function') throw new TypeError('MutationObserver needs a callback');
            this._cb = cb; this._records = []; this._targets = new Set();
        }
        observe(target, options) {
            const o = Object.assign({}, options);
            if (o.attributeOldValue || o.attributeFilter) o.attributes = o.attributes !== false;
            if (o.characterDataOldValue) o.characterData = o.characterData !== false;
            if (!o.childList && !o.attributes && !o.characterData) throw new TypeError('observe() needs childList, attributes or characterData');
            let list = moRegs.get(target);
            if (!list) moRegs.set(target, list = []);
            const r = list.find((x) => x.observer === this);
            if (r) r.options = o; else list.push({ observer: this, options: o });
            this._targets.add(target);
        }
        disconnect() {
            for (const t of this._targets) {
                const l = moRegs.get(t);
                if (!l) continue;
                const i = l.findIndex((x) => x.observer === this);
                if (i >= 0) l.splice(i, 1);
                if (!l.length) moRegs.delete(t);
            }
            this._targets.clear();
            this._records = [];
        }
        takeRecords() { const r = this._records; this._records = []; return r; }
    }
    function moDeliver() {
        moScheduled = false;
        const list = Array.from(moPending);
        moPending.clear();
        for (const obs of list) {
            const recs = obs.takeRecords();
            if (recs.length) { try { obs._cb.call(obs, recs, obs); } catch (e) { reportError(e); } }
        }
    }
    function moQueue(type, target, f) {
        if (!moRegs.size || !target) return;
        const hit = new Map();
        for (let n = target; n; n = n.parentNode || (n.nodeType === 11 && n.host) || null) {
            const l = moRegs.get(n);
            if (l) for (const r of l) {
                const o = r.options;
                if (n !== target && !o.subtree) continue;
                if (type === 'childList' && !o.childList) continue;
                if (type === 'attributes' && (!o.attributes || (o.attributeFilter && !o.attributeFilter.includes(f.name)))) continue;
                if (type === 'characterData' && !o.characterData) continue;
                if (!hit.has(r.observer)) hit.set(r.observer, o);
            }
        }
        for (const [obs, o] of hit) {
            const keepOld = type === 'attributes' ? o.attributeOldValue : type === 'characterData' ? o.characterDataOldValue : false;
            obs._records.push(new MutationRecord(type, target, keepOld ? f : Object.assign({}, f, { old: undefined })));
            moPending.add(obs);
        }
        if (hit.size && !moScheduled) { moScheduled = true; Promise.resolve().then(moDeliver); }
    }
    G.MutationObserver = G.WebKitMutationObserver = MutationObserver;
    G.MutationRecord = MutationRecord;

    /* ---- the wrapped primitives ---------------------------------------- */
    const PROTOS = [NP, EP2, HEP, CharacterData.prototype, Document.prototype, DocumentFragment.prototype];
    const owner = (name) => PROTOS.find((p) => Object.prototype.hasOwnProperty.call(p, name));
    function wrapFn(name, fn) {
        const p = owner(name);
        if (!p) return;
        const orig = p[name];
        if (typeof orig !== 'function') return;
        Object.defineProperty(p, name, { value: function (...a) { return watching() ? fn.call(this, orig, a) : orig.apply(this, a); }, writable: true, configurable: true });
    }
    function wrapSet(name, fn) {
        const p = owner(name);
        if (!p) return;
        const d = Object.getOwnPropertyDescriptor(p, name);
        if (!d || !d.set) return;
        Object.defineProperty(p, name, {
            get: d.get, enumerable: d.enumerable, configurable: true,
            set(v) { if (watching()) fn.call(this, d.set, v); else d.set.call(this, v); }
        });
    }
    const kids = (n) => (n && n.childNodes ? Array.from(n.childNodes) : []);
    function inserted(parent, nodes) {
        if (!nodes.length || !parent) return;
        const first = nodes[0], last = nodes[nodes.length - 1];
        moQueue('childList', parent, { added: nodes, prev: first.previousSibling, next: last.nextSibling });
        if (parent.isConnected) ceConnected(nodes);
    }
    function removed(parent, nodes, wasConnected, prev, next) {
        if (!nodes.length || !parent) return;
        moQueue('childList', parent, { removed: nodes, prev, next });
        if (wasConnected) ceDisconnected(nodes);
    }
    /* a node being inserted leaves its old parent first */
    function detachFirst(node) {
        if (!node || node.nodeType === 11 || !node.parentNode) return null;
        return { parent: node.parentNode, conn: node.isConnected, prev: node.previousSibling, next: node.nextSibling };
    }
    const toInsert = (node) => (node && node.nodeType === 11 ? kids(node) : node ? [node] : []);

    wrapFn('appendChild', function (orig, [c]) {
        const from = detachFirst(c), nodes = toInsert(c);
        const r = orig.call(this, c);
        if (from) removed(from.parent, [c], from.conn, from.prev, from.next);
        inserted(this, nodes);
        return r;
    });
    wrapFn('insertBefore', function (orig, [c, ref]) {
        const from = detachFirst(c), nodes = toInsert(c);
        const r = orig.call(this, c, ref);
        if (from) removed(from.parent, [c], from.conn, from.prev, from.next);
        inserted(this, nodes);
        return r;
    });
    wrapFn('removeChild', function (orig, [c]) {
        const conn = c && c.isConnected, prev = c && c.previousSibling, next = c && c.nextSibling;
        const r = orig.call(this, c);
        removed(this, [c], conn, prev, next);
        return r;
    });
    wrapFn('replaceChild', function (orig, [c, old]) {
        const from = detachFirst(c), nodes = toInsert(c);
        const conn = old && old.isConnected, prev = old && old.previousSibling, next = old && old.nextSibling;
        const r = orig.call(this, c, old);
        if (from) removed(from.parent, [c], from.conn, from.prev, from.next);
        removed(this, [old], conn, prev, next);
        inserted(this, nodes);
        return r;
    });
    wrapFn('__remove', function (orig, a) {
        const parent = this.parentNode, conn = this.isConnected, prev = this.previousSibling, next = this.nextSibling;
        const r = orig.apply(this, a);
        removed(parent, [this], conn, prev, next);
        return r;
    });
    /* setters and calls that replace a run of children: compare before / after */
    function diffChildren(parent, run) {
        if (!parent) { run(); return; }
        const before = kids(parent), conn = parent.isConnected;
        run();
        const after = kids(parent), was = new Set(before), now = new Set(after);
        removed(parent, before.filter((n) => !now.has(n)), conn, null, null);
        inserted(parent, after.filter((n) => !was.has(n)));
    }
    wrapSet('innerHTML', function (set, v) { diffChildren(this, () => set.call(this, v)); });
    wrapSet('outerHTML', function (set, v) { diffChildren(this.parentNode, () => set.call(this, v)); });
    wrapFn('insertAdjacentHTML', function (orig, a) {
        const pos = String(a[0]).toLowerCase();
        const parent = pos === 'beforebegin' || pos === 'afterend' ? this.parentNode : this;
        let r;
        diffChildren(parent, () => { r = orig.apply(this, a); });
        return r;
    });
    wrapSet('textContent', function (set, v) {
        if (this.nodeType === 3 || this.nodeType === 8) {
            const old = this.data;
            set.call(this, v);
            moQueue('characterData', this, { old });
        } else diffChildren(this, () => set.call(this, v));
    });
    wrapSet('data', function (set, v) {
        const old = this.data;
        set.call(this, v);
        moQueue('characterData', this, { old });
    });
    function attrChanged(el, name, old) {
        const now = el.getAttribute(name);
        moQueue('attributes', el, { name, old });
        if (ceState.get(el) === 'custom') {
            const def = ceDefs.get(el.localName);
            if (def && def.attrs.has(name)) ceCall(el, 'attributeChangedCallback', name, old, now, null);
        }
    }
    wrapFn('setAttribute', function (orig, [n, v]) {
        const name = String(n).toLowerCase(), old = this.getAttribute(name);
        const r = orig.call(this, n, v);
        attrChanged(this, name, old);
        return r;
    });
    wrapFn('removeAttribute', function (orig, [n]) {
        const name = String(n).toLowerCase(), old = this.getAttribute(name);
        const r = orig.call(this, n);
        if (old !== null) attrChanged(this, name, old);
        return r;
    });

    /* window's own EventTarget methods where ShadyDOM and others look for them */
    G.Window.prototype = Object.create(EventTarget.prototype);
    for (const m of ['addEventListener', 'removeEventListener', 'dispatchEvent'])
        Object.defineProperty(G.Window.prototype, m, { value: G[m], writable: true, configurable: true });
    Object.defineProperty(G.Window.prototype, 'constructor', { value: G.Window, writable: true, configurable: true });
    try { Object.setPrototypeOf(G, G.Window.prototype); } catch (e) { /* the global keeps Object.prototype */ }
}

/* `window.foo` lookups of element ids are not provided (named access). */
})();
