-- 27/10/2026: shaders the players' launchers share (the community's shader packs). Once, on a database made before:
--   npx wrangler d1 execute otavix-launcher --remote --file=migracao-shaders.sql
CREATE TABLE IF NOT EXISTS shaders (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  jogo TEXT NOT NULL,
  arquivo TEXT NOT NULL,       -- the cache file's name in the game (one shader, or a batch of a storage file's records)
  install TEXT NOT NULL,
  ip TEXT NOT NULL,
  tamanho INTEGER NOT NULL,
  quando INTEGER NOT NULL,
  dados BLOB NOT NULL          -- under D1's 2 MB a value
);
CREATE UNIQUE INDEX IF NOT EXISTS shaders_um ON shaders (jogo, arquivo, install, tamanho);
CREATE INDEX IF NOT EXISTS shaders_jogo ON shaders (jogo, id);
CREATE INDEX IF NOT EXISTS shaders_install ON shaders (install, quando);
