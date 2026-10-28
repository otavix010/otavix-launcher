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
  if (req.method === 'POST' && p === '/v1/report/foto') return foto(req, env, url);
  if (req.method === 'POST' && p === '/v1/pedidos') return pedir(req, env);
  if (req.method === 'GET' && p === '/v1/pedidos/meus') return meusPedidos(req, env);
  if (req.method === 'DELETE' && p === '/v1/pedidos') return retirar(req, env);
  if (req.method === 'GET' && p === '/v1/ranking') return ranking(env, url, false);
  if (req.method === 'POST' && p === '/v1/desempenho') return desempenho(req, env);
  if (req.method === 'GET' && p === '/v1/desempenho') return resumo(env, url);
  if (req.method === 'DELETE' && p === '/v1/desempenho') return apagar(req, env);
  if (req.method === 'GET' && p === '/v1/respostas') return respostas(req, env);
  if (req.method === 'GET' && p === '/v1/recado') return recado(env);
  // 28/10: o envio era de um shader por pedido (até 3000 por jogador) e estourou o limite diário do D1. Até o
  // envio em lote, o servidor responde 429 sem tocar no banco: o launcher para no primeiro 429 e tenta outro dia,
  // então cada jogador gasta um pedido por sessão. SHADERS_ABERTO = '1' (variável do worker) reabre.
  if (req.method === 'POST' && p === '/v1/shaders') {
    if (env.SHADERS_ABERTO !== '1') return new Response('envio de shaders pausado', { status: 429 });
    return shaderEnviar(req, env, url);
  }
  if (req.method === 'GET' && p.startsWith('/v1/github/')) return github(env, url);
  if (p.startsWith('/v1/admin/')) {
    if (!env.ADMIN_TOKEN || req.headers.get('Authorization') !== 'Bearer ' + env.ADMIN_TOKEN) return text(401, 'chave inválida');
    if (req.method === 'GET' && p === '/v1/admin/ranking') return ranking(env, url, true);
    if (req.method === 'GET' && p === '/v1/admin/comentarios') return comentarios(env, url);
    if (req.method === 'GET' && p === '/v1/admin/reports') return reports(env);
    if (req.method === 'GET' && p === '/v1/admin/anexo') return anexo(env, url);
    if (req.method === 'POST' && p === '/v1/admin/ocultar') return ocultar(req, env);
    if (req.method === 'POST' && p === '/v1/admin/renomear') return renomear(req, env);
    if (req.method === 'POST' && p === '/v1/admin/juntar') return juntar(req, env);
    if (req.method === 'POST' && p === '/v1/admin/responder') return responder(req, env);
    if (req.method === 'POST' && p === '/v1/admin/remover') return remover(req, env);
    if (req.method === 'POST' && p === '/v1/admin/portado') return portado(req, env);
    if (req.method === 'POST' && p === '/v1/admin/recado') return novoRecado(req, env);
    if (req.method === 'GET' && p === '/v1/admin/shaders') return shadersLista(env, url);
    if (req.method === 'GET' && p === '/v1/admin/shader') return shaderBaixar(env, url);
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

// ------------------------------------------------------------------------------------------------ GitHub

// GitHub for the launchers, through here: without a login GitHub's API answers 60 requests an hour per address, many
// players share one (carrier-grade NAT), a launcher asks about itself and each port, and Cloudflare's own addresses
// are often over that limit too. So a branch's newest commit comes from git's public endpoint (no API), a release from
// the API or else from its public page, and each answer is kept in the database for a few minutes; the last good one
// is served, however old, while GitHub refuses. Only the project's repositories and the requests the launcher makes.
// GITHUB_TOKEN (optional secret) raises the API's limit.
const GH_OWNERS = ['otavix010'];
const GH_UA = { 'User-Agent': 'otavix-launcher-servidor' };
const GH_RULES = [
  // the path, how long an answer is fresh (s), how it is looked up
  { re: /^repos\/(([\w.-]+)\/[\w.-]+)\/releases\/latest$/, ttl: 600, how: 'latest' },
  { re: /^repos\/(([\w.-]+)\/[\w.-]+)\/releases$/, ttl: 600, how: 'api' },
  { re: /^repos\/(([\w.-]+)\/[\w.-]+)\/commits\/([\w./-]+)$/, ttl: 300, how: 'branch' },
  { re: /^repos\/(([\w.-]+)\/[\w.-]+)\/compare\/[0-9a-f]{7,40}\.\.\.[0-9a-f]{7,40}$/, ttl: 30 * 86400, how: 'files' },
];

function jsonResponse(status, corpo) {
  return new Response(corpo, { status, headers: { 'Content-Type': 'application/json; charset=utf-8' } });
}

async function githubApi(env, key) {
  const headers = { ...GH_UA, Accept: 'application/vnd.github+json' };
  if (env.GITHUB_TOKEN) headers.Authorization = 'Bearer ' + env.GITHUB_TOKEN;
  try {
    const res = await fetch('https://api.github.com/' + key, { headers });
    return { status: res.status, corpo: await res.text() };
  } catch {
    return null;
  }
}

// a branch's newest commit from git's smart HTTP (refs/heads/<branch>), as the API's {"sha": ...}
async function githubBranch(repo, branch) {
  try {
    const res = await fetch(`https://github.com/${repo}.git/info/refs?service=git-upload-pack`, { headers: GH_UA });
    if (!res.ok) return { status: res.status === 404 ? 404 : 502, corpo: '{"message":"repositório não encontrado"}' };
    const refs = await res.text();
    const m = refs.match(new RegExp('([0-9a-f]{40}) refs/heads/' + branch.replace(/[.*+?^${}()|[\]\\]/g, '\\$&') + '[\\0\\n]'));
    return m ? { status: 200, corpo: JSON.stringify({ sha: m[1] }) } : { status: 404, corpo: '{"message":"branch não encontrado"}' };
  } catch {
    return null;
  }
}

// the newest release from its public page, as the API's fields the launcher reads (each asset's size and digest
// before its browser_download_url)
async function githubReleasePage(repo) {
  try {
    const r = await fetch(`https://github.com/${repo}/releases/latest`, { redirect: 'manual', headers: GH_UA });
    const tag = ((r.headers.get('Location') || '').match(/\/releases\/tag\/([^/?#]+)$/) || [])[1];
    if (!tag) return r.status === 302 || r.status === 404 ? { status: 404, corpo: '{"message":"Not Found"}' } : null;
    const page = await fetch(`https://github.com/${repo}/releases/expanded_assets/${tag}`, { headers: GH_UA });
    if (!page.ok) return null;
    const html = await page.text();
    const assets = [];
    for (const li of html.split('<li')) {
      const href = (li.match(/href="(\/[^"]+\/releases\/download\/[^"]+)"/) || [])[1];
      if (!href) continue;
      const url = 'https://github.com' + href.replace(/&amp;/g, '&');
      const head = await fetch(url, { method: 'HEAD', headers: GH_UA });
      assets.push({
        name: decodeURIComponent(url.split('/').pop()),
        size: Number(head.headers.get('Content-Length') || 0),
        digest: (li.match(/sha256:[0-9a-f]{64}/) || [null])[0],
        browser_download_url: url,
      });
    }
    const t = decodeURIComponent(tag);
    return { status: 200, corpo: JSON.stringify({ tag_name: t, html_url: `https://github.com/${repo}/releases/tag/${tag}`, assets }) };
  } catch {
    return null;
  }
}

async function github(env, url) {
  const path = url.pathname.slice('/v1/github/'.length);
  if (url.search && !/^\?per_page=\d{1,3}$/.test(url.search)) return text(400, 'consulta não permitida');
  const rule = GH_RULES.find((r) => r.re.test(path));
  const m = rule ? path.match(rule.re) : null;
  if (!m || !GH_OWNERS.includes(m[2].toLowerCase())) return text(403, 'fora do projeto');
  const key = path + url.search;
  const t = now();
  const row = await env.DB.prepare('SELECT status, corpo, quando FROM github WHERE caminho = ?').bind(key).first();
  if (row && t - row.quando < rule.ttl) return jsonResponse(row.status, row.corpo);
  let got = null;
  if (rule.how === 'branch') {
    got = await githubBranch(m[1], m[3]);
  } else {
    got = await githubApi(env, key);
    if (got && got.status === 200 && rule.how === 'files')
      got.corpo = JSON.stringify({ files: (JSON.parse(got.corpo).files || []).map((f) => ({ filename: f.filename })) });
    if (rule.how === 'latest' && !(got && (got.status === 200 || got.status === 404))) got = (await githubReleasePage(m[1])) || got;
  }
  if (got && (got.status === 200 || got.status === 404)) {
    if (got.corpo.length < 1_500_000)
      await env.DB.prepare(
        'INSERT INTO github (caminho, status, corpo, quando) VALUES (?, ?, ?, ?) ' +
          'ON CONFLICT (caminho) DO UPDATE SET status = excluded.status, corpo = excluded.corpo, quando = excluded.quando'
      ).bind(key, got.status, got.corpo, t).run();
    return jsonResponse(got.status, got.corpo);
  }
  // GitHub refused (its limit) or did not answer: the last copy, however old
  if (row) return jsonResponse(row.status, row.corpo);
  return jsonResponse(got ? got.status : 502, got ? got.corpo : '{"message":"GitHub não respondeu"}');
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
  const id = res.meta.last_row_id;
  // the game's log and settings, kept with it (until 1.0.23 they only went by mail, and without mail set up were lost)
  const textos = [];
  if (r.log) textos.push([nomeArquivo(r.log_nome, 'log.txt'), String(r.log).slice(0, 1_000_000)]);
  if (r.config) textos.push(['settings.toml', String(r.config).slice(0, 200_000)]);
  if (textos.length)
    await env.DB.batch(textos.map(([nome, s]) => {
      const bytes = new TextEncoder().encode(s);
      return env.DB.prepare('INSERT INTO anexos (report, nome, tipo, tamanho, dados) VALUES (?, ?, ?, ?, ?)')
        .bind(id, nome, 'text/plain; charset=utf-8', bytes.length, bytes);
    }));
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
    await env.DB.prepare('UPDATE reports SET enviado = ? WHERE id = ?').bind(mailed, id).run();
  }
  // the report's id after a tab: the launcher sends its pictures to it (older launchers read only the status)
  return text(200, (mailed ? 'ok' : 'ok guardado') + '\t' + id);
}

// a file name from the player: no path, nothing the admin's list or a file system would choke on
function nomeArquivo(v, padrao) {
  const n = f(v).replace(/^.*[\\/]/, '').replace(/[|;:*?"<>]/g, '_').trim().slice(0, 80);
  return n || padrao;
}

// a picture for a report just sent from the same launcher: the bytes as they are (JPEG or PNG), up to 4 a report,
// within 30 minutes of it
async function foto(req, env, url) {
  const inst = install(req);
  const id = Number(url.searchParams.get('report'));
  if (!inst || !Number.isInteger(id) || id <= 0) return text(400, 'qual report?');
  const rep = await env.DB.prepare('SELECT install, quando FROM reports WHERE id = ?').bind(id).first();
  if (!rep || rep.install !== inst || now() - rep.quando > 1800) return text(403, 'report de outro launcher ou antigo');
  const len = Number(req.headers.get('Content-Length') || 0);
  if (len > 1_900_000) return text(413, 'foto grande demais');
  const bytes = new Uint8Array(await req.arrayBuffer());
  if (bytes.length > 1_900_000) return text(413, 'foto grande demais');
  const jpeg = bytes[0] === 0xff && bytes[1] === 0xd8 && bytes[2] === 0xff;
  const png = bytes[0] === 0x89 && bytes[1] === 0x50 && bytes[2] === 0x4e && bytes[3] === 0x47;
  if (!jpeg && !png) return text(415, 'só JPEG ou PNG');
  const n = await env.DB.prepare("SELECT COUNT(*) AS n FROM anexos WHERE report = ? AND tipo LIKE 'image/%'").bind(id).first();
  if (n.n >= 4) return text(429, 'até 4 fotos por report');
  const nome = nomeArquivo(url.searchParams.get('nome'), `foto${n.n + 1}.${jpeg ? 'jpg' : 'png'}`);
  await env.DB.prepare('INSERT INTO anexos (report, nome, tipo, tamanho, dados) VALUES (?, ?, ?, ?, ?)')
    .bind(id, nome, jpeg ? 'image/jpeg' : 'image/png', bytes.length, bytes).run();
  return text(200, 'ok');
}

// one attachment, for the owner's launcher
async function anexo(env, url) {
  const id = Number(url.searchParams.get('id'));
  if (!Number.isInteger(id) || id <= 0) return text(400, 'qual anexo?');
  const a = await env.DB.prepare('SELECT nome, tipo, dados FROM anexos WHERE id = ?').bind(id).first();
  if (!a) return text(404, 'não encontrado');
  // D1 hands a BLOB back as an ArrayBuffer or, in older runtimes, an array of numbers
  const dados = a.dados instanceof ArrayBuffer ? a.dados : new Uint8Array(a.dados);
  return new Response(dados, {
    status: 200,
    headers: { 'Content-Type': a.tipo, 'Content-Disposition': `attachment; filename="${a.nome.replace(/"/g, '')}"` },
  });
}

async function reports(env) {
  const rows = await env.DB.prepare(
    'SELECT id, quando, titulo, plataforma, categoria, contato, descricao, enviado, resposta, sistema, ' +
      "(SELECT group_concat(a.id || '|' || a.nome || '|' || a.tamanho, ';') FROM anexos a WHERE a.report = r.id) AS anexos " +
      'FROM reports r ORDER BY quando DESC LIMIT 100'
  ).all();
  // the id, the answer, the attachments ("id|name|bytes;...") and the computer last: older launchers read the fields
  // before them by position
  return text(200, rows.results.map((r) =>
    [r.quando, r.titulo, r.plataforma, r.categoria, r.contato, r.descricao.slice(0, 400), r.enviado, r.id, r.resposta,
      r.anexos || '', resumoSistema(r.sistema)]
      .map(f).join('\t')).join('\n'));
}

// the report's computer in one line: processor, video card, screen, launcher's port version
function resumoSistema(s) {
  try {
    const o = JSON.parse(s || '{}');
    return ['cpu', 'gpu', 'display', 'port'].map((k) => o[k]).filter(Boolean).join(' · ');
  } catch {
    return '';
  }
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

// ------------------------------------------------------------------------------------------------ notice

// the owner's notice to everyone, the newest active one: a line "id<TAB>when", then its text as written (line breaks
// kept); nothing when there is none
async function recado(env) {
  const r = await env.DB.prepare('SELECT id, texto, criado FROM recados WHERE ativo = 1 ORDER BY id DESC LIMIT 1').first();
  return text(200, r ? r.id + '\t' + r.criado + '\n' + r.texto : '');
}

// a new notice for every launcher (the ones before it stop showing); an empty text takes the current one back
async function novoRecado(req, env) {
  const r = await body(req, 8000);
  const texto = String(r.texto || '').replace(/\r\n/g, '\n').trim().slice(0, 2000);
  await env.DB.prepare('UPDATE recados SET ativo = 0 WHERE ativo = 1').run();
  if (!texto) return text(200, '0');
  const row = await env.DB.prepare('INSERT INTO recados (texto, criado) VALUES (?, ?) RETURNING id').bind(texto, now()).first();
  return text(200, String(row.id));
}

// ------------------------------------------------------------------------------------------------ shaders

// a cache file a player's launcher shares (opted in): one shader of a game, or a batch of the records it made for a
// shader storage file; kept as sent, checked and merged into the game's pack on the owner's PC before anyone gets it
async function shaderEnviar(req, env, url) {
  const inst = install(req);
  if (!inst) return text(400, 'sem identificador');
  const jogo = String(url.searchParams.get('jogo') || '');
  const arquivo = String(url.searchParams.get('arquivo') || '');
  if (!/^[a-z0-9-]{1,48}$/.test(jogo) || !/^[A-Za-z0-9_.-]{1,96}$/.test(arquivo)) return text(400, 'jogo ou arquivo inválido');
  const len = Number(req.headers.get('Content-Length') || 0);
  if (len > 1_900_000) return text(413, 'grande demais');
  const bytes = new Uint8Array(await req.arrayBuffer());
  if (!bytes.length || bytes.length > 1_900_000) return text(413, 'vazio ou grande demais');
  // a launcher sends at most 3 000 files or 40 MB an hour
  const desde = now() - 3600;
  const uso = await env.DB.prepare('SELECT COUNT(*) AS n, COALESCE(SUM(tamanho), 0) AS b FROM shaders WHERE install = ? AND quando > ?')
    .bind(inst, desde).first();
  if (uso.n >= 3000 || uso.b + bytes.length > 40_000_000) return text(429, 'muitos envios nesta hora');
  await env.DB.prepare('INSERT OR IGNORE INTO shaders (jogo, arquivo, install, ip, tamanho, quando, dados) VALUES (?, ?, ?, ?, ?, ?, ?)')
    .bind(jogo, arquivo, inst, await ipHash(req), bytes.length, now(), bytes).run();
  return text(200, 'ok');
}

// what was shared, after an id: id, game, file, sender, size, when (oldest first, a page of 500)
async function shadersLista(env, url) {
  const desde = Number(url.searchParams.get('desde') || 0);
  const jogo = String(url.searchParams.get('jogo') || '');
  const rows = jogo
    ? await env.DB.prepare('SELECT id, jogo, arquivo, install, tamanho, quando FROM shaders WHERE jogo = ? AND id > ? ORDER BY id LIMIT 500').bind(jogo, desde).all()
    : await env.DB.prepare('SELECT id, jogo, arquivo, install, tamanho, quando FROM shaders WHERE id > ? ORDER BY id LIMIT 500').bind(desde).all();
  return text(200, rows.results.map((r) => [r.id, r.jogo, r.arquivo, r.install, r.tamanho, r.quando].map(f).join('\t')).join('\n'));
}

async function shaderBaixar(env, url) {
  const id = Number(url.searchParams.get('id'));
  if (!Number.isInteger(id) || id <= 0) return text(400, 'qual shader?');
  const r = await env.DB.prepare('SELECT dados FROM shaders WHERE id = ?').bind(id).first();
  if (!r) return text(404, 'não encontrado');
  const dados = r.dados instanceof ArrayBuffer ? r.dados : new Uint8Array(r.dados);
  return new Response(dados, { status: 200, headers: { 'Content-Type': 'application/octet-stream' } });
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
  const feito = await env.DB.prepare('SELECT portado FROM jogos WHERE console = ? AND chave = ?').bind(console, k).first();
  if (feito && feito.portado) return text(200, 'portado');
  return text(200, res.meta.changes ? 'ok' : 'ja');
}

// this launcher's own requests: console, key, name, when, comment, cover, ported (when; 0 = not yet)
async function meusPedidos(req, env) {
  const id = install(req);
  if (!id) return text(400, 'launcher sem identificação');
  const rows = await env.DB.prepare(
    'SELECT v.console, v.chave, j.nome, v.quando, v.comentario, j.capa, j.portado FROM votos v ' +
      'JOIN jogos j ON j.console = v.console AND j.chave = v.chave WHERE v.install = ? ORDER BY v.quando DESC LIMIT 200'
  ).bind(id).all();
  return text(200, rows.results.map((r) => [r.console, r.chave, r.nome, r.quando, r.comentario, r.capa, r.portado].map(f).join('\t')).join('\n'));
}

// a player takes back their own request; a game nobody asks for any more leaves the list with it
async function retirar(req, env) {
  const id = install(req);
  if (!id) return text(400, 'launcher sem identificação');
  const r = await body(req, 4000);
  const console = String(r.console || '');
  const k = await resolve(env, console, String(r.chave || ''));
  const res = await env.DB.prepare('DELETE FROM votos WHERE install = ? AND console = ? AND chave = ?').bind(id, console, k).run();
  await apagarSemVotos(env, console, k);
  return text(200, res.meta.changes ? 'ok' : 'nada');
}

// a game with no votes left goes, with the names merged into it
async function apagarSemVotos(env, console, k) {
  const n = await env.DB.prepare('SELECT COUNT(*) AS n FROM votos WHERE console = ? AND chave = ?').bind(console, k).first();
  if (n.n === 0)
    await env.DB.prepare('DELETE FROM jogos WHERE console = ? AND (chave = ? OR alias_of = ?)').bind(console, k, k).run();
}

// the owner removes a request for good: its votes, the game and the names merged into it
async function remover(req, env) {
  const r = await body(req, 4000);
  const console = String(r.console || ''), k = String(r.chave || '');
  if (!console || !k) return text(400, 'pedido inválido');
  await env.DB.batch([
    env.DB.prepare('DELETE FROM votos WHERE console = ? AND chave = ?').bind(console, k),
    env.DB.prepare('DELETE FROM jogos WHERE console = ? AND (chave = ? OR alias_of = ?)').bind(console, k, k),
  ]);
  return text(200, 'ok');
}

// console, key, name, votes, cover, ported (for the owner: console, key, name, votes, hidden, comments, cover, ported);
// ported = when the owner marked it (0 = not yet), last so that older launchers read the rest as before
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
    `SELECT j.console, j.chave, j.nome, j.oculto, j.capa, j.portado, COUNT(*) AS votos, SUM(v.comentario <> '') AS coments
       FROM votos v JOIN jogos j ON j.console = v.console AND j.chave = v.chave
      WHERE ${where.join(' AND ')} AND j.alias_of IS NULL
      GROUP BY j.console, j.chave ORDER BY votos DESC, j.nome LIMIT ${admin ? 500 : 100}`
  ).bind(...args).all();
  return text(200, rows.results.map((r) =>
    (admin ? [r.console, r.chave, r.nome, r.votos, r.oculto, r.coments, r.capa, r.portado]
           : [r.console, r.chave, r.nome, r.votos, r.capa, r.portado]).map(f).join('\t')).join('\n'));
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

// the owner marks a requested game as ported (or takes the mark back); everyone sees it in the ranking
async function portado(req, env) {
  const r = await body(req, 4000);
  await env.DB.prepare('UPDATE jogos SET portado = ? WHERE console = ? AND chave = ?').bind(r.portado ? now() : 0, r.console, r.chave).run();
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
