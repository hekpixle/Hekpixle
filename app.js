/* ============================================================================
   Hekpixle v0.2 — renderer logic
   - Markdown rendering via marked + highlight.js
   - Math/physics formulas via KaTeX (inline $...$ and block $$...$$)
   - Enhanced animations: neon + soft, mixed
   - Vazirmatn (fa) + Space Grotesk (en) font stacks via CSS
   ============================================================================ */

const $ = (id) => document.getElementById(id);
const h = (tag, cls, text) => {
  const e = document.createElement(tag);
  if (cls) e.className = cls;
  if (text != null) e.textContent = text;
  return e;
};
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

let settings = {};
let chats = [];
let works = [];
let currentId = null;
let busy = false;
let live = null; // the assistant step currently streaming
let expectFirst = false;
let pendingAttachments = []; // files the user attached to the next outgoing message

const uid = () => Date.now().toString(36) + Math.random().toString(36).slice(2, 7);

/* ============================================================================
   Markdown + KaTeX rendering pipeline
   ============================================================================ */

// Configure marked once on load. We mark math placeholders first to protect
// them from marked's escape passes, then restore them as KaTeX-rendered HTML.
const MATH_BLOCK_RE = /\$\$([\s\S]+?)\$\$/g;
const MATH_INLINE_RE = /\$([^\n$][^$\n]*)\$/g;

function escapeHtml(s) {
  return s.replace(/&/g, '&amp;')
          .replace(/</g, '&lt;')
          .replace(/>/g, '&gt;')
          .replace(/"/g, '&quot;')
          .replace(/'/g, '&#39;');
}

function renderMathToHtml(tex, displayMode) {
  try {
    return katex.renderToString(tex, { displayMode, throwOnError: false, output: 'html' });
  } catch (e) {
    return '<code style="color:#b3402f">' + escapeHtml(tex) + '</code>';
  }
}

// Renders a chunk of text to safe HTML.
// 1. Pull out math first ($$...$$ and $...$), replace with placeholders.
// 2. Let marked parse the rest (escaping happens inside).
// 3. Restore math placeholders as KaTeX-rendered HTML.
// 4. Highlight code blocks via highlight.js.
function renderMarkdown(text) {
  if (!text) return '';
  const mathStore = [];

  // Block math first
  let s = text.replace(MATH_BLOCK_RE, (_, tex) => {
    mathStore.push(renderMathToHtml(tex.trim(), true));
    return `\u2402\u2402MATH${mathStore.length - 1}\u2402\u2402`;
  });

  // Inline math
  s = s.replace(MATH_INLINE_RE, (_, tex) => {
    mathStore.push(renderMathToHtml(tex.trim(), false));
    return `\u2401MATH${mathStore.length - 1}\u2401`;
  });

  // Marked parse
  const html = marked.parse(s, { breaks: true, gfm: true });

  // Restore placeholders
  const out = html
    .replace(/\u2402\u2402MATH(\d+)\u2402\u2402/g, (_, i) => mathStore[+i] || '')
    .replace(/\u2401MATH(\d+)\u2401/g, (_, i) => mathStore[+i] || '');

  return out;
}

// After inserting rendered HTML, walk the container and:
// - highlight all <pre><code> blocks via hljs
// - add copy buttons
// - make all <a> tags open externally (handled in click handler)
function postProcessRendered(root) {
  if (!root) return;
  // Highlight code blocks
  root.querySelectorAll('pre code').forEach((block) => {
    try {
      // marked already added language-xxx class; let hljs auto-detect if missing
      if (!block.className || !block.className.match(/language-/)) {
        block.classList.add('hljs');
      }
      hljs.highlightElement(block);
    } catch {}
  });

  // Add copy button to each <pre>
  root.querySelectorAll('pre').forEach((pre) => {
    if (pre.querySelector('.code-copy')) return;
    const btn = document.createElement('button');
    btn.className = 'code-copy';
    btn.textContent = 'copy';
    btn.type = 'button';
    btn.addEventListener('click', (e) => {
      e.preventDefault();
      const code = pre.querySelector('code');
      const txt = code ? code.textContent : '';
      navigator.clipboard.writeText(txt).then(() => {
        btn.textContent = 'copied ✓';
        btn.classList.add('copied');
        setTimeout(() => {
          btn.textContent = 'copy';
          btn.classList.remove('copied');
        }, 1400);
      }).catch(() => {
        btn.textContent = 'failed';
        setTimeout(() => { btn.textContent = 'copy'; }, 1400);
      });
    });
    pre.appendChild(btn);
  });

  // External links — already styled; clicks handled via delegation in init()
}

// Configure marked once
function configureMarked() {
  if (!window.marked) return;
  marked.setOptions({
    breaks: true,
    gfm: true,
    smartLists: true,
  });
}
configureMarked();

/* ============================================================================
   Helpers
   ============================================================================ */
let _systemDarkMq = null;
function resolveTheme(t) {
  // Resolve 'system' to either 'light' or 'dark' based on OS preference
  if (t !== 'system') return t === 'dark' ? 'dark' : 'light';
  if (!_systemDarkMq) {
    _systemDarkMq = window.matchMedia('(prefers-color-scheme: dark)');
    // Re-apply theme when the OS preference changes
    _systemDarkMq.addEventListener('change', () => {
      if ((settings.theme || 'light') === 'system') applyTheme('system');
    });
  }
  return _systemDarkMq.matches ? 'dark' : 'light';
}
function applyTheme(t) {
  const resolved = resolveTheme(t);
  document.body.dataset.theme = resolved;
  document.body.dataset.themePref = t;  // 'light' | 'dark' | 'system'
  // Swap highlight.js theme to match
  const themeLink = document.getElementById('hljsTheme');
  if (themeLink) {
    themeLink.href = resolved === 'dark'
      ? 'lib/highlight/github-dark.min.css'
      : 'lib/highlight/github.min.css';
  }
  // Update the active theme card in the Appearance tab
  document.querySelectorAll('.theme-card').forEach((card) => {
    card.classList.toggle('active', card.dataset.theme === t);
  });
}
function applyReduceMotion(on) {
  document.body.classList.toggle('reduce-motion', !!on);
}

const current = () => chats.find((c) => c.id === currentId);
const persistChats = () => window.hek.saveChats(chats);
const persistWorks = () => window.hek.saveWorks(works);

const nearBottom = () => {
  const b = $('messages');
  return b.scrollHeight - b.scrollTop - b.clientHeight < 140;
};
const stick = () => { const b = $('messages'); b.scrollTop = b.scrollHeight; };

function showView(name) {
  $('viewChat').classList.toggle('hidden', name !== 'chat');
  $('viewWorks').classList.toggle('hidden', name !== 'works');
  $('btnWorks').classList.toggle('active', name === 'works');
  if (name === 'works') renderWorks();
}

/* ============================================================================
   Chat list
   ============================================================================ */
function renderChatList() {
  const box = $('chatList');
  box.textContent = '';
  chats.forEach((c) => {
    const el = h('div', 'chat-item' + (c.id === currentId ? ' active' : ''));
    const t = h('span', 't', c.title || 'New chat');
    t.dir = 'auto';
    const x = h('button', 'x', '×');
    x.title = 'Delete chat';
    x.onclick = (e) => {
      e.stopPropagation();
      if (busy) return;
      chats = chats.filter((k) => k.id !== c.id);
      if (currentId === c.id) currentId = chats[0]?.id || null;
      persistChats();
      renderChatList();
      renderMessages();
    };
    el.append(t, x);
    el.onclick = () => {
      if (busy) return;
      currentId = c.id;
      showView('chat');
      renderChatList();
      renderMessages();
    };
    box.appendChild(el);
  });
}

/* ============================================================================
   Message building blocks
   ============================================================================ */
function makeThink(text, isLive) {
  const d = h('details', 'think' + (isLive ? ' live' : ''));
  d.open = true;
  const s = h('summary');
  const orb = h('span', 'orb');
  const label = h('span', 'label', isLive ? 'در حال فکر کردن…' : 'فکر کرد');
  s.append(orb, label);
  const body = h('div', 'think-body', text || '');
  body.dir = 'auto';
  d.append(s, body);
  return { d, body, label, done: !isLive };
}

function fmtArgs(a) {
  try {
    const o = typeof a === 'string' ? JSON.parse(a || '{}') : a;
    return Object.entries(o || {})
      .map(([k, v]) => (k === 'content' ? `content(${String(v).length} chars)` : `${k}: ${v}`))
      .join('   ');
  } catch {
    return String(a || '');
  }
}

function makeChip(name, args, status) {
  const c = h('div', 'tool-chip ' + (status || ''), `⚙ ${name}   ${fmtArgs(args)}`);
  return c;
}

const statusOf = (r) => (r.startsWith('DENIED') ? 'denied' : r.startsWith('ERROR') ? 'error' : 'ok');

function addError(text) {
  const row = h('div', 'msg-row assistant error enter');
  row.append(h('div', 'bubble', text));
  $('messages').appendChild(row);
}

// Builds the assistant bubble. If the message has content, it is rendered
// as Markdown + KaTeX. Reasoning is shown as plain text (pre-wrap).
function renderAssistant(m, results) {
  const col = h('div', 'turn');
  if (m.reasoning) col.append(makeThink(m.reasoning, false).d);
  if (m.content) {
    const bubble = h('div', 'bubble md');
    bubble.dir = 'auto';
    bubble.innerHTML = renderMarkdown(m.content);
    postProcessRendered(bubble);    col.append(bubble);
  }
  (m.tool_calls || []).forEach((tc) => col.append(makeChip(tc.function.name, tc.function.arguments, results[tc.id])));
  if (!col.children.length) return;
  const row = h('div', 'msg-row assistant enter');
  row.append(col);
  $('messages').appendChild(row);
}

function renderMessages() {
  const box = $('messages');
  box.textContent = '';
  live = null;
  const c = current();
  const msgs = c ? c.messages : [];
  $('composerWrap').classList.toggle('empty', msgs.length === 0);
  const results = {};
  msgs.forEach((m) => { if (m.role === 'tool') results[m.tool_call_id] = statusOf(m.content || ''); });
  msgs.forEach((m) => {
    if (m.role === 'user') {
      const row = h('div', 'msg-row user enter');
      // User messages stay as plain text (safer; preserves whitespace)
      const bubble = h('div', 'bubble');
      bubble.dir = 'auto';
      bubble.textContent = m.content;
      row.append(bubble);
      box.appendChild(row);
    } else if (m.role === 'assistant') {
      renderAssistant(m, results);
      if (m.error) addError(m.error);
    }
  });
  stick();
}

function newChat() {
  if (busy) return;
  const c = { id: uid(), title: '', messages: [] };
  chats.unshift(c);
  currentId = c.id;
  persistChats();
  showView('chat');
  renderChatList();
  renderMessages();
  $('input').focus();
}

/* ============================================================================
   Live streaming (thinking / text / tools / permission)
   ============================================================================ */

// Throttle: re-render the bubble's Markdown at most once per animation frame
let renderTick = null;
let composerLive = false;

function setComposerLive(on) {
  const comp = $('composer');
  if (!comp) return;
  if (on && !composerLive) {
    comp.classList.add('live');
    composerLive = true;
  } else if (!on && composerLive) {
    comp.classList.remove('live');
    composerLive = false;
  }
}

function beginStep() {
  const row = h('div', 'msg-row assistant enter');
  const col = h('div', 'turn');
  const think = makeThink('', true);
  col.append(think.d);
  row.append(col);
  $('messages').appendChild(row);
  live = { row, col, think, answer: null, chips: {} };
  setComposerLive(true);
  stick();
}

function finishThink() {
  if (!live || live.think.done) return;
  live.think.done = true;
  if (!live.think.body.textContent.trim()) {
    live.think.d.remove();
  } else {
    live.think.d.classList.remove('live');
    live.think.label.textContent = 'فکر کرد';
  }
}

function addPermission(ev) {
  const danger = /⚠️/.test(ev.title || '');
  const card = h('div', 'perm' + (danger ? ' danger' : ''));
  card.append(h('div', 'pt', (danger ? '' : '🔒 ') + ev.title));
  const code = h('code', null, ev.detail || '');
  card.append(code);
  const acts = h('div', 'acts');
  const ok = h('button', 'btn sm', 'اجازه می‌دم');
  const always = ev.canAlways && ev.tool ? h('button', 'btn sm ghost', 'همیشه برای «' + ev.tool + '»') : null;
  const no = h('button', 'btn sm ghost', 'نه');
  const state = h('div', 'state');
  const answer = (v) => {
    card.classList.add('done', v ? 'allowed' : 'rejected');
    state.textContent = v === 'always' ? '✓ همیشه مجاز شد (از Settings → Tools قابل پاک‌کردنه)' : v ? '✓ اجازه داده شد' : '✕ رد شد';
    window.hek.answerPermission(ev.id, v);
  };
  ok.onclick = () => answer(true);
  if (always) always.onclick = () => answer('always');
  no.onclick = () => answer(false);
  acts.append(ok);
  if (always) acts.append(always);
  acts.append(no);
  card.append(acts, state);
  live.col.append(card);
}

// Agent asks the user a question (clarify tool)
function addClarify(ev) {
  const card = h('div', 'perm clarify');
  card.append(h('div', 'pt', '❓ ' + ev.question));
  const acts = h('div', 'acts');
  const state = h('div', 'state');
  const send = (text) => {
    if (card.classList.contains('done')) return;
    card.classList.add('done', 'allowed');
    state.textContent = '✓ پاسخ: ' + text;
    window.hek.answerClarify(ev.id, text);
  };
  (ev.choices || []).forEach((c) => { const b = h('button', 'btn sm ghost', c); b.onclick = () => send(c); acts.append(b); });
  const inp = h('input'); inp.type = 'text'; inp.placeholder = 'پاسخ خودت…'; inp.dir = 'auto'; inp.className = 'clarify-input';
  const go = h('button', 'btn sm', 'ارسال');
  go.onclick = () => { if (inp.value.trim()) send(inp.value.trim()); };
  inp.addEventListener('keydown', (e) => { if (e.key === 'Enter' && inp.value.trim()) send(inp.value.trim()); });
  card.append(acts, h('div', 'acts').appendChild(inp).parentNode, state);
  card.lastChild.previousSibling.append(go);
  (live ? live.col : $('messages')).append(card);
}

function renderTodoCard(todos) {
  if (!live) return;
  if (!live.todoCard) { live.todoCard = h('div', 'todo-card'); live.col.append(live.todoCard); }
  const icon = { completed: '✅', in_progress: '🔄', cancelled: '⛔', pending: '⬜' };
  live.todoCard.innerHTML = '';
  live.todoCard.append(h('div', 'todo-title', '📋 برنامه'));
  (todos || []).forEach((t) => live.todoCard.append(h('div', 'todo-item ' + t.status, (icon[t.status] || '⬜') + ' ' + t.content)));
}

// Live-update the assistant bubble's Markdown. Called from the 'content'
// event. We accumulate the streaming text, and throttle the actual render.
// Appends a typing caret (.stream-caret) and marks the bubble as .streaming
// so the Gemini-style sweep + caret blink animations kick in.
function flushRender() {
  renderTick = null;
  if (!live || !live.answer) return;
  const html = renderMarkdown(live.rawText);
  // Mark streaming + append the caret at the very end (outside any inline
  // element so it shows up consistently after the last word).
  live.answer.innerHTML = html + '<span class="stream-caret" aria-hidden="true"></span>';
  live.answer.classList.add('streaming');
  postProcessRendered(live.answer);
  // Keep scrolled to bottom if user is near bottom
  if (nearBottom()) stick();
}

function onAgentEvent(ev) {
  const stickIt = nearBottom();
  switch (ev.type) {
    case 'start-step':
      if (expectFirst) expectFirst = false; // placeholder already shown
      else beginStep();
      break;
    case 'reasoning':
      if (!live) break;
      live.think.body.textContent += ev.delta;
      if (live.think.body.scrollHeight > live.think.body.clientHeight) {
        live.think.body.scrollTop = live.think.body.scrollHeight;
      }
      break;
    case 'content':
      if (!live) break;
      finishThink();
      if (!live.answer) {
        live.answer = h('div', 'bubble md');
        live.answer.dir = 'auto';
        live.col.append(live.answer);
        live.rawText = '';
      }
      live.rawText += ev.delta;
      // Throttle re-render to avoid jank
      if (renderTick) cancelAnimationFrame(renderTick);
      renderTick = requestAnimationFrame(flushRender);
      break;
    case 'end-step':
      finishThink();
      // Final flush for any pending delta
      if (renderTick) { cancelAnimationFrame(renderTick); renderTick = null; }
      if (live && live.answer && live.rawText != null) {
        live.answer.innerHTML = renderMarkdown(live.rawText);
        live.answer.classList.remove('streaming');
        // Remove any leftover caret
        const caret = live.answer.querySelector('.stream-caret');
        if (caret) caret.remove();
        postProcessRendered(live.answer);
      }
      setComposerLive(false);
      break;
    case 'tool': {
      if (!live) break;
      finishThink();
      const chip = makeChip(ev.name, ev.args, 'running');
      live.chips[ev.callId] = chip;
      live.col.append(chip);
      break;
    }
    case 'permission':
      if (!live) break;
      finishThink();
      addPermission(ev);
      break;
    case 'tool-result': {
      const chip = live?.chips[ev.callId];
      if (chip) chip.className = 'tool-chip ' + ev.status;
      break;
    }
    case 'image': {
      // thumbnail of what the agent is looking at (screenshot / browser)
      if (!live) break;
      const img = document.createElement('img');
      img.className = 'agent-shot';
      img.src = ev.src;
      img.alt = 'screenshot';
      img.onclick = () => { const w = window.open(); if (w) w.document.write('<img style="max-width:100%" src="' + ev.src + '">'); };
      const chip = live.chips[ev.callId];
      if (chip && chip.after) chip.after(img); else live.col.append(img);
      break;
    }
    case 'todo':
      renderTodoCard(ev.todos);
      break;
    case 'clarify':
      finishThink();
      addClarify(ev);
      break;
    case 'cron-result':
      addSystemMessage('⏰ **' + ev.name + '** (' + ev.status + ')\n\n' + ev.text);
      break;
  }
  if (stickIt || ev.type === 'permission' || ev.type === 'clarify') stick();
}

/* ============================================================================
   Send animation — neon trail + soft easing
   ============================================================================ */
async function playSendAnimation(text, from, to, row) {
  const btn = $('btnSend');
  const fly = h('div', 'fly');
  const txt = h('div', 'txt', text);
  fly.append(txt);
  document.body.append(fly);
  Object.assign(fly.style, {
    left: from.left + 'px', top: from.top + 'px',
    width: from.width + 'px', height: from.height + 'px',
    borderRadius: '50%', opacity: '0'
  });
  txt.style.width = Math.max(10, to.width - 28) + 'px';

  try {
    // 1) the send button slides right + fades
    const slide = btn.animate(
      [
        { transform: 'translateX(0) scale(1)', opacity: 1 },
        { transform: 'translateX(80px) scale(.85)', opacity: 0 }
      ],
      { duration: 280, easing: 'cubic-bezier(.6,0,.9,.5)', fill: 'forwards' }
    );

    // 2) the button pops back with a slight spring
    const back = (async () => {
      await slide.finished;
      btn.animate(
        [
          { transform: 'scale(.3) translateY(8px)', opacity: 0 },
          { transform: 'scale(1.08) translateY(0)', opacity: 1, offset: .7 },
          { transform: 'scale(1) translateY(0)', opacity: 1 }
        ],
        { duration: 360, delay: 80, fill: 'forwards', easing: 'cubic-bezier(.2,1.4,.4,1)' }
      );
      slide.cancel();
    })();

    // 3) the flying bubble shoots up with neon trail
    await sleep(120);
    fly.style.opacity = '1';
    const flight = fly.animate(
      [
        {
          left: from.left + 'px', top: from.top + 'px',
          width: from.width + 'px', height: from.height + 'px',
          borderRadius: '50%',
          boxShadow: '0 0 28px 4px rgba(43,47,51,.18)'
        },
        {
          left: to.left + 'px', top: to.top + 'px',
          width: to.width + 'px', height: to.height + 'px',
          borderRadius: '16px',
          boxShadow: '0 14px 36px rgba(0,0,0,.25)'
        }
      ],
      { duration: 560, easing: 'cubic-bezier(.22,1,.36,1)', fill: 'forwards' }
    );
    txt.animate(
      [
        { opacity: 0, transform: 'translateY(4px)' },
        { opacity: 0, offset: 0.4 },
        { opacity: 1, transform: 'translateY(0)' }
      ],
      { duration: 560, fill: 'forwards', easing: 'cubic-bezier(.2,.8,.2,1)' }
    );
    await flight.finished;
    await back;
  } catch {
    /* animations are cosmetic */
  } finally {
    row.classList.remove('pending-fly');
    fly.remove();
  }
}

/* ============================================================================
   Slash commands (inspired by Hermes Agent)
   Commands: /new, /clear, /retry, /undo, /help, /model, /compress
   ============================================================================ */
function handleSlashCommand(text) {
  const parts = text.split(/\s+/);
  const cmd = parts[0].toLowerCase();
  const c = current();

  switch (cmd) {
    case '/new':
    case '/reset':
      newChat();
      return true;

    case '/clear':
      if (c) {
        c.messages = [];
        persistChats();
        renderMessages();
        renderChatList();
      }
      return true;

    case '/retry':
      if (!c || c.messages.length === 0) {
        addSystemMessage('چیزی برای retry نیست.');
        return true;
      }
      // Remove last assistant message, keep last user message, re-send
      while (c.messages.length > 0) {
        const last = c.messages[c.messages.length - 1];
        if (last.role === 'assistant') {
          c.messages.pop();
        } else if (last.role === 'user') {
          break;
        } else {
          c.messages.pop();
        }
      }
      const lastUser = c.messages[c.messages.length - 1];
      if (lastUser && lastUser.role === 'user') {
        const userText = lastUser.content;
        c.messages.pop(); // remove the user message (will be re-added by send())
        persistChats();
        renderMessages();
        // Re-send the message
        $('input').value = userText;
        autoGrow();
        setTimeout(() => send(), 100);
      } else {
        addSystemMessage('پیام کاربری برای retry پیدا نشد.');
      }
      return true;

    case '/undo':
      if (!c || c.messages.length === 0) {
        addSystemMessage('چیزی برای undo نیست.');
        return true;
      }
      // Remove last exchange: pop assistant(s) + user
      while (c.messages.length > 0) {
        const last = c.messages[c.messages.length - 1];
        c.messages.pop();
        if (last.role === 'user') break;
      }
      persistChats();
      renderMessages();
      renderChatList();
      addSystemMessage('✓ آخرین پیام حذف شد.');
      return true;

    case '/help':
      addSystemMessage(
        '**دستورات قابل استفاده:**\n\n' +
        '• `/new` یا `/reset` — شروع چت جدید\n' +
        '• `/clear` — پاک‌کردن پیام‌های چت فعلی\n' +
        '• `/retry` — ارسال دوباره‌ی آخرین پیام (جواب قبلی حذف می‌شه)\n' +
        '• `/undo` — حذف آخرین تبادل (پیام شما + جواب ایجنت)\n' +
        '• `/rollback` — بازگردوندن آخرین فایلی که ایجنت تغییر داده\n' +
        '• `/model` — نمایش مدل فعلی\n' +
        '• `/compress` — خلاصه‌کردن بخش قدیمی چت (کاهش مصرف توکن)\n' +
        '• `/tools` — وضعیت ابزارها و MCP\n' +
        '• `/skills` — فهرست skills\n' +
        '• `/cron` — کارهای زمان‌بندی‌شده\n' +
        '• `@path/to/file` — محتوای فایل به پیام اضافه می‌شه\n' +
        '• `/help` — این پیام'
      );
      return true;

    case '/rollback':
      $('input').value = 'با ابزار file_history آخرین checkpointهای فایل‌هایی که تغییر دادی رو نشون بده و بپرس کدوم رو برگردونم؛ اگه فقط یکی بود با file_restore برش گردون.';
      autoGrow();
      setTimeout(() => send(), 100);
      return true;

    case '/tools':
      window.hek.toolsInfo().then((i) => addSystemMessage(
        '**Toolsets:**\n\n' + i.toolsets.map((t) => `• \`${t.name}\` — ${t.enabled ? 'روشن' : 'خاموش'} (${t.count} ابزار)`).join('\n') +
        '\n\n**MCP:** ' + (i.mcp.length ? i.mcp.map((m) => `${m.name} ${m.ok ? '✓ ' + m.tools + ' tools' : '✕ ' + m.error}`).join(' | ') : 'سروری تنظیم نشده') +
        '\n**Plugins:** ' + (i.plugins.loaded.length ? i.plugins.loaded.join(', ') : 'نصب نشده') + (i.plugins.errors.length ? '\n⚠️ ' + i.plugins.errors.join('\n') : '')));
      return true;

    case '/skills':
      window.hek.listSkills().then((l) => addSystemMessage(l.length ? '**Skills:**\n\n' + l.map((x) => `• \`${x.name}\` — ${x.description}`).join('\n') : 'هنوز skill ای نیست. از ایجنت بخواه یکی بسازه.'));
      return true;

    case '/cron':
      window.hek.cronList().then((r) => addSystemMessage(r.jobs.length
        ? '**کارهای زمان‌بندی‌شده:**\n\n' + r.jobs.map((j) => `• \`${j.id}\` **${j.name}** — ${j.enabled ? '▶' : '⏸'} بعدی: ${j.nextRunAt ? new Date(j.nextRunAt).toLocaleString() : '—'} → ${j.deliver}`).join('\n')
        : 'کاری زمان‌بندی نشده. مثال: «هر صبح ساعت ۹ خلاصه‌ی اخبار رو بفرست تلگرام».'));
      return true;

    case '/model':
      addSystemMessage(
        `**مدل فعلی:** \`${settings.model || 'تنظیم نشده'}\`\n` +
        `**Base URL:** \`${settings.baseUrl || 'تنظیم نشده'}\`\n` +
        `برای تغییر مدل، به Settings → General برو.`
      );
      return true;

    case '/compress':
      if (!c || c.messages.length < 14) {
        addSystemMessage('چت هنوز کوتاهه — فشرده‌سازی لازم نیست.');
        return true;
      }
      addSystemMessage('⏳ در حال خلاصه‌کردن بخش قدیمی چت...');
      window.hek.compressChat(c.messages).then((r) => {
        if (r.error) { addSystemMessage('⚠️ ' + r.error); return; }
        const before = c.messages.length;
        c.messages = r.messages;
        persistChats();
        renderMessages();
        addSystemMessage(`✓ ${before} پیام به ${r.messages.length} پیام فشرده شد (بخش قدیمی خلاصه شد).`);
      });
      return true;

    default:
      // Unknown command — return false so it gets sent to the LLM as normal text
      return false;
  }
}

function addSystemMessage(text) {
  const row = h('div', 'msg-row assistant enter');
  const col = h('div', 'turn');
  const bubble = h('div', 'bubble md');
  bubble.dir = 'auto';
  bubble.innerHTML = window.renderMarkdown ? window.renderMarkdown(text) : escapeHtml(text);
  if (typeof postProcessRendered === 'function') postProcessRendered(bubble);
  col.append(bubble);
  row.append(col);
  $('messages').appendChild(row);
  stick();
}

/* ============================================================================
   Sending — with attachments + stop button + slash commands
   ============================================================================ */
async function send() {
  const text = $('input').value.trim();
  if (!text && pendingAttachments.length === 0) return;
  if (busy) return;

  // Slash commands — handle locally without sending to LLM (like Hermes)
  if (text.startsWith('/') && pendingAttachments.length === 0) {
    const handled = handleSlashCommand(text);
    if (handled) {
      $('input').value = '';
      autoGrow();
      return;
    }
    // If not a known command, fall through and send to LLM as normal text
  }

  if (!current()) newChat();
  const c = current();
  busy = true;
  setSendButtonMode('stopping');

  // ---- Context References (@ mentions) — expand @path to file content inline ----
  let finalText = text;
  const atPattern = /@([A-Za-z]:[\\/][^\s@]+|[.\/~][^\s@]+|[^\s@]+\.[a-zA-Z]{1,5})/g;
  const atMatches = [...text.matchAll(atPattern)];
  if (atMatches.length > 0 && window.hek?.readInline) {
    const expandedParts = [];
    let lastIndex = 0;
    for (const match of atMatches) {
      const fullPath = match[1];
      const matchStart = match.index;
      const matchEnd = matchStart + match[0].length;
      expandedParts.push(text.slice(lastIndex, matchStart));
      try {
        const result = await window.hek.readInline(fullPath);
        if (result?.content) {
          expandedParts.push(`[Content of ${fullPath}]:\n\`\`\`\n${result.content.slice(0, 4000)}\n\`\`\``);
        } else {
          expandedParts.push(`@[${fullPath} — ${result?.error || 'read failed'}]`);
        }
      } catch (e) {
        expandedParts.push(`@[${fullPath} — read error: ${e.message}]`);
      }
      lastIndex = matchEnd;
    }
    expandedParts.push(text.slice(lastIndex));
    finalText = expandedParts.join('');
  }

  // Trigger the Gemini-style spark ring on the send button
  const btn = $('btnSend');
  let sparkRing = btn.querySelector('.spark-ring');
  if (!sparkRing) {
    sparkRing = h('span', 'spark-ring');
    btn.appendChild(sparkRing);
  }
  btn.classList.remove('firing');
  void btn.offsetWidth;
  btn.classList.add('firing');
  setTimeout(() => btn.classList.remove('firing'), 700);

  const btnRect = btn.getBoundingClientRect();
  const comp = $('composer');
  const compBefore = comp.getBoundingClientRect();

  // Build the user message text — include attachment paths so the agent knows
  let userText = text;
  const atts = pendingAttachments.slice();
  if (atts.length > 0) {
    const lines = atts.map((a) => `• ${a.path} (${a.kind}, ${formatFileSize(a.size)})`);
    userText = (userText ? userText + '\n\n' : '') +
      '[Attached files — use read_file to read them if needed:]\n' + lines.join('\n');
  }

  // Use finalText (with @ mentions expanded) if it was modified, else use userText
  if (finalText !== text) userText = finalText;

  $('input').value = '';
  autoGrow();

  c.messages.push({ role: 'user', content: userText });
  if (!c.title) c.title = text.slice(0, 40) || (atts[0] ? atts[0].name : 'New chat');
  renderChatList();
  renderMessages();

  // Clear the attachment chips (they're now baked into the message)
  pendingAttachments = [];
  renderAttachments();

  const rows = $('messages').querySelectorAll('.msg-row.user');
  const row = rows[rows.length - 1];
  row.classList.add('pending-fly');
  stick();

  // composer glides down from the middle of the page on the first message
  const compAfter = comp.getBoundingClientRect();
  const dy = compBefore.top - compAfter.top;
  if (Math.abs(dy) > 1) {
    comp.animate(
      [{ transform: `translateY(${dy}px)` }, { transform: 'translateY(0)' }],
      { duration: 500, easing: 'cubic-bezier(.2,.8,.2,1)' }
    );
  }

  const target = row.querySelector('.bubble').getBoundingClientRect();
  playSendAnimation(text || atts[0]?.name || '…', btnRect, target, row);

  expectFirst = true;
  beginStep();

  const payload = c.messages.filter((x) => x.role && !x.error).map(({ error, reasoning, ...rest }) => rest);

  let res;
  try {
    res = await window.hek.runAgent(payload);
  } catch (e) {
    res = { error: String(e.message || e) };
  }
  expectFirst = false;

  if (res.added?.length) c.messages.push(...res.added);
  if (res.error && res.error !== 'STOPPED') {
    c.messages.push({ role: 'assistant', content: '', error: res.error });
  } else if (res.error === 'STOPPED') {
    c.messages.push({ role: 'assistant', content: '⏹ متوقف شد.' });
  }
  persistChats();
  busy = false;
  setSendButtonMode('send');
  setComposerLive(false);
  renderChatList();
  if (current() && current().id === c.id) renderMessages();
  $('input').focus();
}

// Toggle the send button between send and stop states
function setSendButtonMode(mode) {
  const btn = $('btnSend');
  if (!btn) return;
  if (mode === 'stopping') {
    btn.classList.add('stopping');
    btn.disabled = false;
    btn.title = 'Stop generation';
  } else {
    btn.classList.remove('stopping');
    btn.disabled = false;
    btn.title = 'Send';
  }
}

// User clicked the stop button — abort the current agent run
async function stopGeneration() {
  try {
    await window.hek.stopAgent();
  } catch {}
}

// ---- Attachment handling ----
const KIND_ICONS = {
  image: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><rect x="3" y="3" width="18" height="18" rx="2"/><circle cx="9" cy="9" r="2"/><path d="m21 15-5-5L5 21"/></svg>',
  video: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="m22 8-6 4 6 4V8Z"/><rect x="2" y="6" width="14" height="12" rx="2"/></svg>',
  audio: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M9 18V5l12-2v13"/><circle cx="6" cy="18" r="3"/><circle cx="18" cy="16" r="3"/></svg>',
  code: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><polyline points="16 18 22 12 16 6"/><polyline points="8 6 2 12 8 18"/></svg>',
  document: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8Z"/><path d="M14 2v6h6"/></svg>',
  archive: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M21 8v13H3V8M1 3h22v5H1zM10 12h4"/></svg>',
  file: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8Z"/><path d="M14 2v6h6"/></svg>',
};

function formatFileSize(bytes) {
  if (!bytes) return '0 B';
  const u = ['B', 'KB', 'MB', 'GB', 'TB'];
  let v = bytes, i = 0;
  while (v >= 1024 && i < u.length - 1) { v /= 1024; i++; }
  return v.toFixed(v < 10 ? 1 : 0) + ' ' + u[i];
}

function renderAttachments() {
  const box = $('attachments');
  if (!box) return;
  box.textContent = '';
  pendingAttachments.forEach((att, idx) => {
    const chip = h('div', 'att-chip kind-' + att.kind);
    const icon = h('span', 'att-icon');
    icon.innerHTML = KIND_ICONS[att.kind] || KIND_ICONS.file;
    const name = h('span', 'att-name', att.name);
    name.title = att.path;
    const size = h('span', 'att-size', formatFileSize(att.size));
    const rm = h('button', 'att-remove', '×');
    rm.title = 'Remove attachment';
    rm.type = 'button';
    rm.onclick = () => {
      pendingAttachments.splice(idx, 1);
      renderAttachments();
    };
    chip.append(icon, name, size, rm);
    box.appendChild(chip);
  });
}

async function pickAttachments() {
  try {
    const result = await window.hek.pickFiles();
    if (result?.files?.length) {
      // Avoid duplicates
      for (const f of result.files) {
        if (!pendingAttachments.find((a) => a.path === f.path)) {
          pendingAttachments.push(f);
        }
      }
      renderAttachments();
    }
  } catch (e) {
    console.error('pickFiles failed:', e);
  }
}

function autoGrow() {
  const i = $('input');
  i.style.height = 'auto';
  i.style.height = Math.min(i.scrollHeight, 200) + 'px';
}

/* ============================================================================
   Works view
   ============================================================================ */
function renderWorks() {
  const g = $('worksGrid');
  g.textContent = '';
  if (!works.length) {
    g.appendChild(h('div', 'empty-note', 'هنوز پروژه‌ای نساختی.'));
    return;
  }
  works.forEach((w) => {
    const card = h('div', 'work-card');
    const t = h('h4', null, w.name);
    t.dir = 'auto';
    card.appendChild(t);
    if (w.desc) {
      const p = h('p', null, w.desc);
      p.dir = 'auto';
      card.appendChild(p);
    }
    if (w.path) card.appendChild(h('code', null, w.path));
    const row = h('div', 'row');
    const open = h('button', 'btn sm', 'چت جدید با این پروژه');
    open.onclick = () => {
      newChat();
      $('input').value = `روی پروژه «${w.name}» کار می‌کنیم.` + (w.path ? ` مسیر پوشه: ${w.path}` : '') + '\n';
      autoGrow();
    };
    const del = h('button', 'btn sm ghost', 'حذف');
    del.onclick = () => {
      works = works.filter((k) => k.id !== w.id);
      persistWorks();
      renderWorks();
    };
    row.append(open, del);
    card.appendChild(row);
    g.appendChild(card);
  });
}

/* ============================================================================
   Init
   ============================================================================ */
async function init() {
  settings = await window.hek.getSettings();
  chats = await window.hek.loadChats();
  works = await window.hek.loadWorks();
  applyTheme(settings.theme);
  applyReduceMotion(settings.reduceMotion);

  // Inject the extra aurora blob for richer Gemini-style background depth
  if (!document.querySelector('.aurora-extra')) {
    const aurora = h('div', 'aurora-extra');
    document.body.appendChild(aurora);
  }

  // Pre-create the spark-ring inside the send button so it's ready to fire
  const btn = $('btnSend');
  if (btn && !btn.querySelector('.spark-ring')) {
    btn.appendChild(h('span', 'spark-ring'));
  }

  currentId = chats[0]?.id || null;
  renderChatList();
  renderMessages();
  showView('chat');

  window.hek.onEvent(onAgentEvent);

  $('btnNewChat').onclick = newChat;
  $('btnWorks').onclick = () => showView('works');
  // Send button — acts as both Send (when idle) and Stop (when busy)
  $('btnSend').onclick = () => {
    if (busy) {
      stopGeneration();
    } else {
      send();
    }
  };
  // Attachment (paperclip) button
  $('btnAttach').onclick = pickAttachments;  $('input').addEventListener('input', autoGrow);
  $('input').addEventListener('keydown', (e) => {
    if (e.key === 'Enter' && !e.shiftKey && !e.isComposing) { e.preventDefault(); send(); }
  });

  // ---- Settings: tab switching + theme picker + about ----
  function switchTab(name) {
    document.querySelectorAll('.settings-tab').forEach((b) => {
      b.classList.toggle('active', b.dataset.tab === name);
    });
    document.querySelectorAll('.settings-panel').forEach((p) => {
      p.classList.toggle('active', p.dataset.panel === name);
    });
  }
  document.querySelectorAll('.settings-tab').forEach((b) => {
    b.onclick = () => switchTab(b.dataset.tab);
  });
  // Theme picker (Appearance tab) — click sets the hidden input + live-previews
  document.querySelectorAll('.theme-card').forEach((card) => {
    card.onclick = () => {
      const t = card.dataset.theme;
      $('setTheme').value = t;
      applyTheme(t);  // live preview (not saved until Save is clicked)
    };
  });
  $('setReduceMotion').onchange = (e) => {
    applyReduceMotion(e.target.checked);  // live preview
  };
  // Close (X) and Cancel both close without saving
  $('setClose').onclick = () => {
    // Revert any live-previewed changes
    applyTheme(settings.theme);
    applyReduceMotion(settings.reduceMotion);
    $('modalSettings').classList.add('hidden');
  };
  // Search box — filter tabs by name (simple substring match)
  $('settingsSearch').addEventListener('input', (e) => {
    const q = e.target.value.trim().toLowerCase();
    document.querySelectorAll('.settings-tab').forEach((b) => {
      const label = b.querySelector('span')?.textContent.toLowerCase() || '';
      const match = !q || label.includes(q);
      b.style.display = match ? '' : 'none';
    });
  });

  // Open settings
  $('btnSettings').onclick = () => {
    $('setKey').value = settings.apiKey || '';
    $('setBase').value = settings.baseUrl || '';
    $('setModel').value = settings.model || '';
    $('setLlmProxy').value = settings.llmProxyUrl || '';
    $('setPersonality').value = settings.personality || '';
    $('setFallbackBase').value = settings.fallbackBaseUrl || '';
    $('setFallbackKey').value = settings.fallbackApiKey || '';
    $('setFallbackModel').value = settings.fallbackModel || '';
    $('setTheme').value = settings.theme || 'light';
    applyTheme(settings.theme);  // syncs theme-card active state
    const tg = settings.telegram || {};
    $('setTgToken').value = tg.botToken || '';
    $('setTgProxy').value = tg.proxyUrl || '';
    $('setTgUserIds').value = Array.isArray(tg.allowedUserIds)
      ? tg.allowedUserIds.join(', ')
      : (tg.allowedUserIds || '');
    $('setTgAutoStart').checked = !!tg.autoStart;
    $('setTgAutoAllowReads').checked = tg.autoAllowReads !== false;
    $('setTgAutoAllowWrites').checked = !!tg.autoAllowWrites;
    $('setTgOnlyMentioned').checked = tg.onlyWhenMentioned !== false;
    $('setReduceMotion').checked = !!settings.reduceMotion;
    fillToolsSettings();
    $('tgTestResult').textContent = '';
    $('tgTestResult').className = 'hint';    // Always open on General tab
    switchTab('general');
    $('settingsSearch').value = '';
    document.querySelectorAll('.settings-tab').forEach((b) => b.style.display = '');
    $('modalSettings').classList.remove('hidden');
    // Populate About tab info
    try {
      const aboutPlatform = document.getElementById('aboutPlatform');
      const aboutNode = document.getElementById('aboutNode');
      const aboutTg = document.getElementById('aboutTg');
      if (aboutPlatform) aboutPlatform.textContent = (navigator.platform || 'unknown');
      if (aboutNode) aboutNode.textContent = (window.hek && window.hek.nodeVersion) ? window.hek.nodeVersion() : '20+';
      if (aboutTg) aboutTg.textContent = tg.botToken ? 'configured' : 'disabled';
    } catch {}
  };
  $('setCancel').onclick = () => {
    // Revert any live-previewed changes
    applyTheme(settings.theme);
    applyReduceMotion(settings.reduceMotion);
    $('modalSettings').classList.add('hidden');
  };
  // Test telegram token — verify the token works before saving. Uses the
  // proxy URL if provided, so the test reflects the actual connection setup.
  $('btnTestTg').onclick = async () => {
    const token = $('setTgToken').value.trim();
    const proxy = $('setTgProxy').value.trim();
    const result = $('tgTestResult');
    if (!token) {
      result.textContent = 'ابتدا توکن رو وارد کن.';
      result.className = 'hint err';
      return;
    }
    result.textContent = 'در حال تست… (اگه پروکسی بذاشتی، از طریق پروکسی تست می‌شه)';
    result.className = 'hint';
    try {
      const r = await window.hek.telegramTest(token, proxy);
      if (r.ok) {
        result.textContent = `✓ متصل. ربات: @${r.username} (ID: ${r.id})`;
        result.className = 'hint ok';
      } else {
        result.textContent = '✕ ' + (r.error || 'ناموفق');
        result.className = 'hint err';
      }
    } catch (e) {
      result.textContent = '✕ ' + (e.message || 'خطا');
      result.className = 'hint err';
    }
  };
  // ---- Tools tab ----
  let toolsLoaded = false;
  function fillToolsSettings() {
    const w = settings.web || {}, m = settings.media || {}, cr = settings.cron || {};
    $('setApproval').value = settings.approvalMode || 'manual';
    $('setMaxSteps').value = settings.maxSteps || 30;
    $('setWorkspace').value = settings.workspaceDir || '';
    $('setCompressAt').value = settings.compressAtChars || 90000;
    $('setBrowserVisible').checked = settings.browserVisible !== false;
    $('setWebProvider').value = w.provider || 'duckduckgo';
    $('setWebKey').value = w.apiKey || '';
    $('setWebSearx').value = w.searxUrl || '';
    $('setWebProxy').value = w.proxyUrl || '';
    $('setVisionMode').value = m.visionMode || 'direct';
    $('setVisionModel').value = m.visionModel || '';
    $('setVisionBase').value = m.visionBaseUrl || '';
    $('setVisionKey').value = m.visionApiKey || '';
    $('setMediaBase').value = m.baseUrl || '';
    $('setMediaKey').value = m.apiKey || '';
    $('setImageModel').value = m.imageModel || '';
    $('setTtsModel').value = m.ttsModel || '';
    $('setTtsVoice').value = m.ttsVoice || '';
    $('setSttModel').value = m.sttModel || '';
    $('setMcp').value = settings.mcpServers && Object.keys(settings.mcpServers).length ? JSON.stringify(settings.mcpServers, null, 2) : '';
    $('setExtraSkills').value = settings.extraSkillsDir || '';
    $('setCronReads').checked = cr.autoAllowReads !== false;
    $('setCronWrites').checked = !!cr.autoAllowWrites;
    const al = settings.alwaysAllow || [];
    $('alwaysList').textContent = al.length ? 'همیشه مجاز: ' + al.join(', ') : 'هیچ ابزاری «همیشه مجاز» نشده.';
    refreshToolsInfo();
  }
  async function refreshToolsInfo() {
    try {
      const i = await window.hek.toolsInfo();
      const box = $('toolsetList');
      box.innerHTML = '';
      i.toolsets.forEach((t) => {
        const l = h('label', 'ts-item');
        const cb = document.createElement('input'); cb.type = 'checkbox'; cb.checked = t.enabled; cb.dataset.ts = t.name;
        l.append(cb, h('span', null, `${t.name} (${t.count})`));
        box.append(l);
      });
      toolsLoaded = true;
      $('mcpStatus').textContent = i.mcp.length ? i.mcp.map((m) => `${m.name}: ${m.ok ? '✓ ' + m.tools + ' tools' : '✕ ' + m.error}`).join('  |  ') : '';
      const sk = await window.hek.listSkills();
      $('skillsInfo').textContent = `${sk.length} skill: ` + (sk.map((x) => x.name).join(', ') || '—') + (i.plugins.loaded.length ? `  |  plugins: ${i.plugins.loaded.join(', ')}` : '') + (i.plugins.errors.length ? `  |  ⚠️ ${i.plugins.errors.join('; ')}` : '');
      const r = await window.hek.cronList();
      const cl = $('cronList');
      cl.innerHTML = '';
      if (!r.jobs.length) cl.append(h('div', 'hint', 'کاری زمان‌بندی نشده.'));
      r.jobs.forEach((j) => {
        const row = h('div', 'cron-row');
        const sched = j.spec.kind === 'cron' ? j.spec.expr : j.spec.kind === 'every' ? 'هر ' + Math.round(j.spec.everyMs / 60000) + ' دقیقه' : 'یک‌بار';
        row.append(h('div', 'cron-info', `${j.enabled ? '▶' : '⏸'} ${j.name} — ${sched} → ${j.deliver}\nبعدی: ${j.nextRunAt ? new Date(j.nextRunAt).toLocaleString() : '—'}${j.lastStatus ? ' | آخرین: ' + j.lastStatus : ''}`));
        const act = (label, action) => { const b = h('button', 'btn sm ghost', label); b.type = 'button'; b.onclick = async () => { b.disabled = true; await window.hek.cronAction({ action, id: j.id }); refreshToolsInfo(); }; return b; };
        row.append(act('اجرا', 'run_now'), act(j.enabled ? 'توقف' : 'ادامه', j.enabled ? 'pause' : 'resume'), act('حذف', 'remove'));
        cl.append(row);
      });
    } catch (e) { $('mcpStatus').textContent = 'خطا در خواندن وضعیت ابزارها: ' + e.message; }
  }
  function collectToolsSettings(mcpServers) {
    const out = {
      approvalMode: $('setApproval').value,
      maxSteps: Math.max(5, Math.min(200, parseInt($('setMaxSteps').value, 10) || 30)),
      workspaceDir: $('setWorkspace').value.trim(),
      compressAtChars: Math.max(20000, parseInt($('setCompressAt').value, 10) || 90000),
      browserVisible: $('setBrowserVisible').checked,
      extraSkillsDir: $('setExtraSkills').value.trim(),
      mcpServers,
      web: { provider: $('setWebProvider').value, apiKey: $('setWebKey').value.trim(), searxUrl: $('setWebSearx').value.trim(), proxyUrl: $('setWebProxy').value.trim() },
      media: {
        visionMode: $('setVisionMode').value, visionModel: $('setVisionModel').value.trim(), visionBaseUrl: $('setVisionBase').value.trim(), visionApiKey: $('setVisionKey').value.trim(),
        baseUrl: $('setMediaBase').value.trim(), apiKey: $('setMediaKey').value.trim(), imageModel: $('setImageModel').value.trim(),
        ttsModel: $('setTtsModel').value.trim(), ttsVoice: $('setTtsVoice').value.trim(), sttModel: $('setSttModel').value.trim(),
      },
      cron: { autoAllowReads: $('setCronReads').checked, autoAllowWrites: $('setCronWrites').checked },
    };
    if (toolsLoaded) {
      const ts = {};
      document.querySelectorAll('#toolsetList input[data-ts]').forEach((cb) => { if (!cb.checked) ts[cb.dataset.ts] = false; });
      out.toolsets = ts;
    }
    if (resetAlwaysFlag) { out.alwaysAllow = []; resetAlwaysFlag = false; }
    return out;
  }
  let resetAlwaysFlag = false;
  $('btnResetAlways').onclick = () => { resetAlwaysFlag = true; $('alwaysList').textContent = 'با «ذخیره» پاک می‌شه.'; };
  $('btnOpenSkills').onclick = () => window.hek.openToolsDir('skills');
  $('btnOpenPlugins').onclick = () => window.hek.openToolsDir('plugins');

  $('setSave').onclick = async () => {
    // Parse user IDs string into an array of numbers
    const userIdsRaw = $('setTgUserIds').value.trim();
    const userIds = userIdsRaw
      ? userIdsRaw.split(/[,\s]+/).map((s) => s.trim()).filter(Boolean).map((s) => parseInt(s, 10)).filter((n) => !isNaN(n))
      : [];
    let mcpServers = settings.mcpServers || {};
    try { const t = $('setMcp').value.trim(); mcpServers = t ? JSON.parse(t) : {}; }
    catch (e) { alert('JSON مربوط به MCP نامعتبره: ' + e.message); return; }
    settings = {
      ...settings,
      ...collectToolsSettings(mcpServers),
      apiKey: $('setKey').value.trim(),
      baseUrl: $('setBase').value.trim() || 'https://api.openai.com/v1',
      model: $('setModel').value.trim() || 'gpt-4o-mini',
      theme: $('setTheme').value,
      reduceMotion: $('setReduceMotion').checked,
      llmProxyUrl: $('setLlmProxy').value.trim(),
      personality: $('setPersonality').value.trim(),
      fallbackBaseUrl: $('setFallbackBase').value.trim(),
      fallbackApiKey: $('setFallbackKey').value.trim(),
      fallbackModel: $('setFallbackModel').value.trim(),
      telegram: {
        ...(settings.telegram || {}),
        botToken: $('setTgToken').value.trim(),
        proxyUrl: $('setTgProxy').value.trim(),
        allowedUserIds: userIds,
        replyOnUnauthorized: true,
        autoStart: $('setTgAutoStart').checked,
        autoAllowReads: $('setTgAutoAllowReads').checked,
        autoAllowWrites: $('setTgAutoAllowWrites').checked,
        onlyWhenMentioned: $('setTgOnlyMentioned').checked,
      },
    };
    await window.hek.setSettings(settings);
    applyTheme(settings.theme);
    applyReduceMotion(settings.reduceMotion);
    $('modalSettings').classList.add('hidden');
    // Refresh Telegram indicator visibility
    updateTgIndicatorVisibility();
  };

  // works
  $('btnAddWork').onclick = () => {
    ['workName', 'workDesc', 'workPath'].forEach((i) => ($(i).value = ''));
    $('modalWork').classList.remove('hidden');
  };
  $('workCancel').onclick = () => $('modalWork').classList.add('hidden');
  $('workSave').onclick = () => {
    const name = $('workName').value.trim();
    if (!name) return;
    works.unshift({ id: uid(), name, desc: $('workDesc').value.trim(), path: $('workPath').value.trim() });
    persistWorks();
    $('modalWork').classList.add('hidden');
    renderWorks();
  };

  // ---- Telegram integration ----
  let tgRunning = false;
  function updateTgIndicatorVisibility() {
    const ind = $('tgIndicator');
    if (!ind) return;
    const hasToken = !!(settings.telegram && settings.telegram.botToken);
    ind.classList.toggle('hidden', !hasToken);
  }
  function updateTgStatusUI(status) {
    const ind = $('tgIndicator');
    if (!ind) return;
    tgRunning = !!status.running;
    ind.classList.toggle('running', !!status.running);
    ind.classList.toggle('error', !!status.lastError && !status.running);
    const state = $('tgState');
    const detail = $('tgErrorDetail');
    const toggle = $('tgToggle');
    if (status.running) {
      state.textContent = status.botUsername ? '@' + status.botUsername : 'running';
      if (detail) detail.textContent = `${status.messageCount || 0} پیام`;
      toggle.textContent = '■';
      toggle.title = 'Stop Telegram bot';
      toggle.disabled = false;
    } else if (status.lastError) {
      state.textContent = 'error';
      if (detail) {
        // Truncate very long errors so they fit in the indicator
        const err = String(status.lastError);
        detail.textContent = err.length > 80 ? err.slice(0, 80) + '…' : err;
      }
      toggle.textContent = '↻';
      toggle.title = 'Retry — ' + status.lastError;
      toggle.disabled = false;
    } else {
      state.textContent = 'stopped';
      if (detail) detail.textContent = '';
      toggle.textContent = '▶';
      toggle.title = 'Start Telegram bot';
      toggle.disabled = false;
    }
  }
  async function refreshTgStatus() {
    try {
      const status = await window.hek.telegramStatus();
      updateTgStatusUI(status);
    } catch {}
  }
  $('tgToggle').onclick = async () => {
    const toggle = $('tgToggle');
    toggle.disabled = true;
    try {
      if (tgRunning) {
        await window.hek.telegramStop();
      } else {
        const res = await window.hek.telegramStart();
        if (!res?.ok) {
          // Show error in the indicator with the actual reason
          updateTgStatusUI({ running: false, lastError: res?.error || 'Failed to start' });
        }
      }
    } catch (e) {
      updateTgStatusUI({ running: false, lastError: String(e.message || e) });
    }
    // Brief delay before re-enabling + refresh
    setTimeout(() => { toggle.disabled = false; refreshTgStatus(); }, 250);
  };
  // Listen for status updates from main (telegram module emits when state changes)
  if (window.hek.onTelegramStatus) {
    window.hek.onTelegramStatus((status) => updateTgStatusUI(status));
  }
  updateTgIndicatorVisibility();
  refreshTgStatus();

  // External link click handler (delegation)
  document.addEventListener('click', (e) => {
    const a = e.target.closest('a');
    if (!a) return;
    const href = a.getAttribute('href');
    if (!href || href.startsWith('#')) return;
    e.preventDefault();
    // Use shell.openExternal via preload if exposed, else fall back to window.open
    if (window.hek && window.hek.openExternal) {
      window.hek.openExternal(href);
    } else {
      try { window.open(href, '_blank', 'noopener,noreferrer'); } catch {}
    }
  });
}

init();
