// Otavix Launcher's server: problem reports (kept, and mailed to the owner through Resend; the owner's answer goes
// back to the launcher that sent it) and port requests with their rankings. A Cloudflare Worker with a D1 database. The launcher sends JSON; answers are lines of tab-separated
// fields.
//
// Secrets (wrangler secret put): ADMIN_TOKEN (the owner's launcher), RESEND_API_KEY and REPORT_TO (mail; without them
// reports are only kept).

const CONSOLES = ['xbox', 'x360', 'ps1', 'ps2', 'ps3', 'psp', 'vita', 'gc', 'wii', 'wiiu', 'nds', '3ds', 'j2me', 'outro'];
const PERIODS = { semana: 7 * 86400, mes: 30 * 86400, tudo: 0 };

export default {
  async fetch(req, env) {
    try {
      return await route(req, env);
    } catch (e) {
      return text(500, 'erro no servidor: ' + (e && e.message ? e.message : e));
    }
  },
};

async function route(req, env) {
  const url = new URL(req.url);
  const p = url.pathname;
  if (req.method === 'GET' && p === '/v1/ping') return text(200, 'ok');
  if (req.method === 'POST' && p === '/v1/report') return report(req, env);
  if (req.method === 'POST' && p === '/v1/pedidos') return pedir(req, env);
  if (req.method === 'GET' && p === '/v1/ranking') return ranking(env, url, false);
  if (req.method === 'POST' && p === '/v1/desempenho') return desempenho(req, env);
  if (req.method === 'GET' && p === '/v1/desempenho') return resumo(env, url);
  if (req.method === 'DELETE' && p === '/v1/desempenho') return apagar(req, env);
  if (req.method === 'GET' && p === '/v1/respostas') return respostas(req, env);
  if (p.startsWith('/v1/admin/')) {
    if (!env.ADMIN_TOKEN || req.headers.get('Authorization') !== 'Bearer ' + env.ADMIN_TOKEN) return text(401, 'chave inválida');
    if (req.method === 'GET' && p === '/v1/admin/ranking') return ranking(env, url, true);
    if (req.method === 'GET' && p === '/v1/admin/comentarios') return comentarios(env, url);
    if (req.method === 'GET' && p === '/v1/admin/reports') return reports(env);
    if (req.method === 'POST' && p === '/v1/admin/ocultar') return ocultar(req, env);
    if (req.method === 'POST' && p === '/v1/admin/renomear') return renomear(req, env);
    if (req.method === 'POST' && p === '/v1/admin/juntar') return juntar(req, env);
    if (req.method === 'POST' && p === '/v1/admin/responder') return responder(req, env);
  }
  return text(404, 'não encontrado');
}

// ------------------------------------------------------------------------------------------------ helpers

function text(status, body) {
  return new Response(body, { status, headers: { 'Content-Type': 'text/plain; charset=utf-8' } });
}

// a field on a tab-separated line: no tabs or line breaks inside
function f(v) {
  return String(v ?? '').replace(/[\t\r\n]+/g, ' ');
}

async function body(req, max) {
  const len = Number(req.headers.get('Content-Length') || 0);
  if (len > max) throw new Error('grande demais');
  const raw = await req.text();
  if (raw.length > max) throw new Error('grande demais');
  return JSON.parse(raw);
}

function now() {
  return Math.floor(Date.now() / 1000);
}

// the player's address is kept only as a hash, to hold back floods
async function ipHash(req) {
  const ip = req.headers.get('CF-Connecting-IP') || 'local';
  const d = await crypto.subtle.digest('SHA-256', new TextEncoder().encode('otavix:' + ip));
  return [...new Uint8Array(d)].slice(0, 12).map((b) => b.toString(16).padStart(2, '0')).join('');
}

function install(req) {
  const id = req.headers.get('X-Otavix-Install') || '';
  return /^[0-9a-f]{32}$/.test(id) ? id : '';
}

// a game's name as a key: no accents, no punctuation, lower case
export function chave(nome) {
  return nome.normalize('NFD').replace(/[̀-ͯ]/g, '').toLowerCase().replace(/[^a-z0-9]+/g, ' ').trim();
}

function b64(s) {
  const bytes = new TextEncoder().encode(s);
  let bin = '';
  for (let i = 0; i < bytes.length; i += 0x8000) bin += String.fromCharCode(...bytes.subarray(i, i + 0x8000));
  return btoa(bin);
}

// ------------------------------------------------------------------------------------------------ reports

async function report(req, env) {
  const r = await body(req, 1_500_000);
  const ip = await ipHash(req);
  const t = now();
  const recent = await env.DB.prepare('SELECT COUNT(*) AS n FROM reports WHERE ip = ? AND quando > ?').bind(ip, t - 3600).first();
  if (recent.n >= 5) return text(429, 'muitos reports nesta hora; tente mais tarde');
  const desc = String(r.descricao || '').slice(0, 8000);
  if (desc.trim().length < 5) return text(400, 'descrição vazia');
  const contato = String(r.contato || '').slice(0, 200);
  const info = r.sistema && typeof r.sistema === 'object' ? r.sistema : {};
  const res = await env.DB.prepare(
    'INSERT INTO reports (quando, ip, install, jogo, titulo, plataforma, versao, categoria, contato, descricao, sistema) ' +
      'VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)'
  )
    .bind(t, ip, install(req), f(r.jogo).slice(0, 80), f(r.titulo).slice(0, 120), f(r.plataforma).slice(0, 40),
      f(r.launcher).slice(0, 40), f(r.categoria).slice(0, 40), contato, desc, JSON.stringify(info).slice(0, 4000))
    .run();
  let mailed = 0;
  if (env.RESEND_API_KEY && env.REPORT_TO) {
    const lines = [
      `Jogo: ${f(r.titulo)} (${f(r.jogo)})`,
      `Plataforma: ${f(r.plataforma)} · launcher ${f(r.launcher)}`,
      `Tipo: ${f(r.categoria)}`,
      `Contato: ${contato || '(não deixou)'}`,
      `Última saída do jogo: ${f(r.ultima_saida)}`,
      '',
      'O que aconteceu:',
      desc,
      '',
      'Computador:',
      ...Object.entries(info).map(([k, v]) => `  ${k}: ${f(v)}`),
    ];
    const attachments = [];
    if (r.log) attachments.push({ filename: f(r.log_nome || 'log.txt'), content: b64(String(r.log).slice(0, 1_000_000)) });
    if (r.config) attachments.push({ filename: 'settings.toml', content: b64(String(r.config).slice(0, 200_000)) });
    const mail = {
      from: 'Otavix Launcher <onboarding@resend.dev>',
      to: [env.REPORT_TO],
      subject: `[Report] ${f(r.titulo)} · ${f(r.categoria)} · ${f(r.plataforma)}`,
      text: lines.join('\n'),
      attachments,
    };
    if (/^[^@\s]+@[^@\s]+\.[^@\s]+$/.test(contato)) mail.reply_to = contato;
    const sent = await fetch('https://api.resend.com/emails', {
      method: 'POST',
      headers: { Authorization: 'Bearer ' + env.RESEND_API_KEY, 'Content-Type': 'application/json' },
      body: JSON.stringify(mail),
    });
    mailed = sent.ok ? 1 : 0;
    await env.DB.prepare('UPDATE reports SET enviado = ? WHERE id = ?').bind(mailed, res.meta.last_row_id).run();
  }
  return text(200, mailed ? 'ok' : 'ok guardado');
}

async function reports(env) {
  const rows = await env.DB.prepare(
    'SELECT id, quando, titulo, plataforma, categoria, contato, descricao, enviado, resposta FROM reports ORDER BY quando DESC LIMIT 100'
  ).all();
  // the id and the answer last: older launchers read the fields before them by position
  return text(200, rows.results.map((r) =>
    [r.quando, r.titulo, r.plataforma, r.categoria, r.contato, r.descricao.slice(0, 400), r.enviado, r.id, r.resposta]
      .map(f).join('\t')).join('\n'));
}

// the owner's answer to a report (empty takes it back)
async function responder(req, env) {
  const r = await body(req, 8000);
  const id = Number(r.id);
  if (!Number.isInteger(id) || id <= 0) return text(400, 'qual report?');
  const resposta = String(r.resposta || '').trim().slice(0, 1000);
  await env.DB.prepare('UPDATE reports SET resposta = ?, respondido = ? WHERE id = ?').bind(resposta, resposta ? now() : 0, id).run();
  return text(200, 'ok');
}

// the answers to this launcher's own reports: id, game, title, kind, the report's start, the answer, when
async function respostas(req, env) {
  const id = install(req);
  if (!id) return text(400, 'sem identificador');
  const rows = await env.DB.prepare(
    "SELECT id, jogo, titulo, categoria, descricao, resposta, respondido FROM reports WHERE install = ? AND resposta <> '' " +
      'ORDER BY respondido DESC LIMIT 50'
  ).bind(id).all();
  return text(200, rows.results.map((r) =>
    [r.id, r.jogo, r.titulo, r.categoria, r.descricao.slice(0, 200), r.resposta, r.respondido].map(f).join('\t')).join('\n'));
}

// ------------------------------------------------------------------------------------------------ performance

// a session's performance (the launcher's ReportJson), from a launcher whose player agreed to share it
async function desempenho(req, env) {
  const id = install(req);
  if (!id) return text(400, 'sem identificador');
  const r = await body(req, 20_000);
  const t = now();
  // 30 an hour from an address, 20 an hour from an install: a launcher sends one a session
  const ip = await ipHash(req);
  const hora = Math.floor(t / 3600);
  await env.DB.prepare('INSERT INTO limite (ip, hora, n) VALUES (?, ?, 1) ON CONFLICT (ip, hora) DO UPDATE SET n = n + 1')
    .bind(ip, hora).run();
  const porIp = await env.DB.prepare('SELECT n FROM limite WHERE ip = ? AND hora = ?').bind(ip, hora).first();
  if (porIp.n > 30) return text(429, 'muitos envios nesta hora');
  const porInstall = await env.DB.prepare('SELECT COUNT(*) AS n FROM desempenho WHERE install = ? AND quando > ?')
    .bind(id, t - 3600).first();
  if (porInstall.n >= 20) return text(429, 'muitos envios nesta hora');
  await env.DB.prepare('DELETE FROM limite WHERE hora < ?').bind(hora - 24).run();

  const m = r.maquina && typeof r.maquina === 'object' ? r.maquina : {};
  const j = r.jogo && typeof r.jogo === 'object' ? r.jogo : {};
  const s = r.sessao && typeof r.sessao === 'object' ? r.sessao : {};
  const num = (v, max) => Math.max(0, Math.min(max, Number(v) || 0));
  const jogo = f(j.id).slice(0, 64);
  if (!/^[a-z0-9-]+$/.test(jogo)) return text(400, 'jogo inválido');
  if (!(num(s.fps_medio, 1000) > 0) || !(num(s.amostras, 1e7) > 0)) return text(400, 'sem fps');
  await env.DB.prepare(
    'INSERT INTO desempenho (quando, install, jogo, port, build, resumo, opcoes, cpu, nucleos, threads, avx512, gpu, ' +
      'vram_mb, driver, ram_gb, windows, minutos, fps_medio, fps_1pct, fps_min, amostras) ' +
      'VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)'
  )
    .bind(t, id, jogo, f(j.port).slice(0, 64), f(j.build).slice(0, 32), f(j.resumo).slice(0, 200),
      JSON.stringify(j.opcoes && typeof j.opcoes === 'object' ? j.opcoes : {}).slice(0, 4000),
      f(m.cpu).slice(0, 120), num(m.nucleos, 1024), num(m.threads, 4096), m.avx512 ? 1 : 0, f(m.gpu).slice(0, 120),
      num(m.vram_mb, 1e7), f(m.driver).slice(0, 40), num(m.ram_gb, 1e5), f(m.windows).slice(0, 40),
      num(s.minutos, 1e5), num(s.fps_medio, 1000), num(s.fps_1pct, 1000), num(s.fps_min, 1000), num(s.amostras, 1e7))
    .run();
  return text(200, 'ok');
}

// a game's performance by computer and options, the last 180 days: gpu, cpu, summary, sessions, computers,
// average fps, 1% low, minimum (the sessions' averages, weighted by the seconds measured)
async function resumo(env, url) {
  const jogo = (url.searchParams.get('jogo') || '').slice(0, 64);
  if (!/^[a-z0-9-]+$/.test(jogo)) return text(400, 'jogo inválido');
  const rows = await env.DB.prepare(
    'SELECT gpu, cpu, resumo, COUNT(*) AS sessoes, COUNT(DISTINCT install) AS maquinas, ' +
      'SUM(fps_medio * amostras) / SUM(amostras) AS medio, SUM(fps_1pct * amostras) / SUM(amostras) AS p1, ' +
      'MIN(fps_min) AS minimo FROM desempenho WHERE jogo = ? AND quando > ? ' +
      'GROUP BY gpu, cpu, resumo ORDER BY maquinas DESC, sessoes DESC LIMIT 60'
  ).bind(jogo, now() - 180 * 86400).all();
  const n = (v) => (Math.round(v * 10) / 10).toFixed(1);
  return text(200, rows.results.map((r) =>
    [r.gpu, r.cpu, r.resumo, r.sessoes, r.maquinas, n(r.medio), n(r.p1), n(r.minimo)].map(f).join('\t')).join('\n'));
}

// everything this install sent, deleted
async function apagar(req, env) {
  const id = install(req);
  if (!id) return text(400, 'sem identificador');
  const res = await env.DB.prepare('DELETE FROM desempenho WHERE install = ?').bind(id).run();
  return text(200, String(res.meta?.changes ?? 0));
}

// ------------------------------------------------------------------------------------------------ port requests

// a key merged into another counts as that one
async function resolve(env, console, k) {
  for (let i = 0; i < 8; i++) {
    const row = await env.DB.prepare('SELECT alias_of FROM jogos WHERE console = ? AND chave = ?').bind(console, k).first();
    if (!row || !row.alias_of) return k;
    k = row.alias_of;
  }
  return k;
}

async function pedir(req, env) {
  const r = await body(req, 20_000);
  const id = install(req);
  if (!id) return text(400, 'launcher sem identificação');
  const console = String(r.console || '');
  if (!CONSOLES.includes(console)) return text(400, 'console desconhecido');
  const nome = String(r.nome || '').replace(/\s+/g, ' ').trim().slice(0, 80);
  let k = chave(nome);
  if (k.length < 2) return text(400, 'nome do jogo curto demais');
  const ip = await ipHash(req);
  const t = now();
  const recent = await env.DB.prepare('SELECT COUNT(*) AS n FROM votos WHERE ip = ? AND quando > ?').bind(ip, t - 3600).first();
  if (recent.n >= 30) return text(429, 'muitos pedidos nesta hora; tente mais tarde');
  k = await resolve(env, console, k);
  // the catalog's box art: a libretro name or gametdb:<region>/<id>
  const capa = f(String(r.capa || '').slice(0, 200));
  await env.DB.prepare('INSERT OR IGNORE INTO jogos (console, chave, nome, capa) VALUES (?, ?, ?, ?)').bind(console, k, nome, capa).run();
  if (capa) await env.DB.prepare("UPDATE jogos SET capa = ? WHERE console = ? AND chave = ? AND capa = ''").bind(capa, console, k).run();
  const res = await env.DB.prepare(
    'INSERT OR IGNORE INTO votos (install, console, chave, comentario, ip, quando) VALUES (?, ?, ?, ?, ?, ?)'
  ).bind(id, console, k, String(r.comentario || '').slice(0, 300), ip, t).run();
  return text(200, res.meta.changes ? 'ok' : 'ja');
}

// console, key, name, votes, cover (for the owner: console, key, name, votes, hidden, comments, cover)
async function ranking(env, url, admin) {
  const since = PERIODS[url.searchParams.get('periodo')] ?? PERIODS.semana;
  const from = since ? now() - since : 0;
  const console = url.searchParams.get('console') || '';
  const where = ['v.quando >= ?'];
  const args = [from];
  if (console && CONSOLES.includes(console)) {
    where.push('j.console = ?');
    args.push(console);
  }
  if (!admin) where.push('j.oculto = 0');
  const rows = await env.DB.prepare(
    `SELECT j.console, j.chave, j.nome, j.oculto, j.capa, COUNT(*) AS votos, SUM(v.comentario <> '') AS coments
       FROM votos v JOIN jogos j ON j.console = v.console AND j.chave = v.chave
      WHERE ${where.join(' AND ')} AND j.alias_of IS NULL
      GROUP BY j.console, j.chave ORDER BY votos DESC, j.nome LIMIT ${admin ? 500 : 100}`
  ).bind(...args).all();
  return text(200, rows.results.map((r) =>
    (admin ? [r.console, r.chave, r.nome, r.votos, r.oculto, r.coments, r.capa] : [r.console, r.chave, r.nome, r.votos, r.capa]).map(f).join('\t')).join('\n'));
}

async function comentarios(env, url) {
  const rows = await env.DB.prepare(
    "SELECT quando, comentario FROM votos WHERE console = ? AND chave = ? AND comentario <> '' ORDER BY quando DESC LIMIT 50"
  ).bind(url.searchParams.get('console') || '', url.searchParams.get('chave') || '').all();
  return text(200, rows.results.map((r) => [r.quando, r.comentario].map(f).join('\t')).join('\n'));
}

async function ocultar(req, env) {
  const r = await body(req, 4000);
  await env.DB.prepare('UPDATE jogos SET oculto = ? WHERE console = ? AND chave = ?').bind(r.oculto ? 1 : 0, r.console, r.chave).run();
  return text(200, 'ok');
}

async function renomear(req, env) {
  const r = await body(req, 4000);
  const nome = String(r.nome || '').replace(/\s+/g, ' ').trim().slice(0, 80);
  if (nome.length < 2) return text(400, 'nome curto demais');
  await env.DB.prepare('UPDATE jogos SET nome = ? WHERE console = ? AND chave = ?').bind(nome, r.console, r.chave).run();
  return text(200, 'ok');
}

// "de" counts as "para" from now on; its votes move over, a player's second vote for the same game dropped
async function juntar(req, env) {
  const r = await body(req, 4000);
  const { console, de, para } = r;
  if (!console || !de || !para || de === para) return text(400, 'pedido inválido');
  await env.DB.batch([
    env.DB.prepare('UPDATE OR IGNORE votos SET chave = ? WHERE console = ? AND chave = ?').bind(para, console, de),
    env.DB.prepare('DELETE FROM votos WHERE console = ? AND chave = ?').bind(console, de),
    env.DB.prepare('UPDATE jogos SET alias_of = ? WHERE console = ? AND chave = ?').bind(para, console, de),
    env.DB.prepare('UPDATE jogos SET alias_of = ? WHERE console = ? AND alias_of = ?').bind(para, console, de),
  ]);
  return text(200, 'ok');
}
