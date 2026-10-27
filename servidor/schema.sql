-- Otavix Launcher's server: requested games, one vote per launcher install and game, and problem reports.
CREATE TABLE IF NOT EXISTS jogos (
  console TEXT NOT NULL,
  chave TEXT NOT NULL,         -- the name without accents or punctuation
  nome TEXT NOT NULL,          -- as shown
  capa TEXT NOT NULL DEFAULT '', -- the catalog's box art
  oculto INTEGER NOT NULL DEFAULT 0,
  portado INTEGER NOT NULL DEFAULT 0, -- when the owner marked it as ported (0 = not yet)
  alias_of TEXT,               -- merged into this key
  PRIMARY KEY (console, chave)
);

CREATE TABLE IF NOT EXISTS votos (
  install TEXT NOT NULL,       -- the launcher's random id
  console TEXT NOT NULL,
  chave TEXT NOT NULL,
  comentario TEXT NOT NULL DEFAULT '',
  ip TEXT NOT NULL,            -- a hash, to hold back floods
  quando INTEGER NOT NULL,
  PRIMARY KEY (install, console, chave)
);
CREATE INDEX IF NOT EXISTS votos_quando ON votos (quando);
CREATE INDEX IF NOT EXISTS votos_ip ON votos (ip, quando);

CREATE TABLE IF NOT EXISTS reports (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  quando INTEGER NOT NULL,
  ip TEXT NOT NULL,
  install TEXT NOT NULL,
  jogo TEXT NOT NULL,
  titulo TEXT NOT NULL,
  plataforma TEXT NOT NULL,
  versao TEXT NOT NULL,
  categoria TEXT NOT NULL,
  contato TEXT NOT NULL,
  descricao TEXT NOT NULL,
  sistema TEXT NOT NULL,
  enviado INTEGER NOT NULL DEFAULT 0,
  resposta TEXT NOT NULL DEFAULT '',     -- the owner's answer, read by the launcher that sent it
  respondido INTEGER NOT NULL DEFAULT 0  -- when it was answered (migracao-respostas.sql for a database made before)
);
CREATE INDEX IF NOT EXISTS reports_ip ON reports (ip, quando);

-- What goes along with a report: the game's log and settings (inside the report) and the player's pictures (each in a
-- request of its own, right after). Kept here: the mail is optional (migracao-anexos.sql for a database made before).
CREATE TABLE IF NOT EXISTS anexos (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  report INTEGER NOT NULL,
  nome TEXT NOT NULL,
  tipo TEXT NOT NULL,          -- image/jpeg, image/png, text/plain
  tamanho INTEGER NOT NULL,
  dados BLOB NOT NULL          -- under D1's 2 MB a value
);
CREATE INDEX IF NOT EXISTS anexos_report ON anexos (report);

-- GitHub's answers for the launchers, kept a few minutes (and served stale while GitHub refuses): see github() in
-- worker.js (migracao-github.sql for a database made before).
CREATE TABLE IF NOT EXISTS github (
  caminho TEXT PRIMARY KEY,    -- the API path asked, with its query
  status INTEGER NOT NULL,
  corpo TEXT NOT NULL,
  quando INTEGER NOT NULL
);

-- Sessions' performance, sent only by launchers whose player agreed: the computer's parts by model, the game and its
-- options, the frame rate. No address is kept with them: the hourly limit counts in a table of its own.
CREATE TABLE IF NOT EXISTS desempenho (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  quando INTEGER NOT NULL,
  install TEXT NOT NULL,       -- the launcher's random id: what the player deletes by
  jogo TEXT NOT NULL,
  port TEXT NOT NULL,
  build TEXT NOT NULL,
  resumo TEXT NOT NULL,        -- the options that matter most, "fps=60;resolution_scale=2": rows are grouped by it
  opcoes TEXT NOT NULL,        -- all the picture options, JSON
  cpu TEXT NOT NULL,
  nucleos INTEGER NOT NULL,
  threads INTEGER NOT NULL,
  avx512 INTEGER NOT NULL,
  gpu TEXT NOT NULL,
  vram_mb INTEGER NOT NULL,
  driver TEXT NOT NULL,
  ram_gb INTEGER NOT NULL,
  windows TEXT NOT NULL,
  minutos REAL NOT NULL,
  fps_medio REAL NOT NULL,
  fps_1pct REAL NOT NULL,
  fps_min REAL NOT NULL,
  amostras INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS desempenho_jogo ON desempenho (jogo, quando);
CREATE INDEX IF NOT EXISTS desempenho_install ON desempenho (install, quando);

-- the hour's count of performance reports per address hash, apart from the reports themselves
CREATE TABLE IF NOT EXISTS limite (
  ip TEXT NOT NULL,
  hora INTEGER NOT NULL,
  n INTEGER NOT NULL,
  PRIMARY KEY (ip, hora)
);

-- the owner's notice to every launcher (the newest active one), shown once as a pop-up
CREATE TABLE IF NOT EXISTS recados (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  texto TEXT NOT NULL,
  criado INTEGER NOT NULL,
  ativo INTEGER NOT NULL DEFAULT 1
);
