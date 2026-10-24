-- Otavix Launcher's server: requested games, one vote per launcher install and game, and problem reports.
CREATE TABLE IF NOT EXISTS jogos (
  console TEXT NOT NULL,
  chave TEXT NOT NULL,         -- the name without accents or punctuation
  nome TEXT NOT NULL,          -- as shown
  capa TEXT NOT NULL DEFAULT '', -- the catalog's box art
  oculto INTEGER NOT NULL DEFAULT 0,
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
  enviado INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS reports_ip ON reports (ip, quando);
