-- 04/11/2026: os shaders encheram o banco (9,5 GB dos 10 GB do D1), quase tudo repetido. O conteúdo passa a ficar no
-- R2, comprimido e uma vez só por hash (shaders/<sha256>); a tabela shaders guarda só quem mandou (dados vazio + hash).
-- Uma vez, no banco existente:
--   npx wrangler d1 execute otavix-launcher --remote --file=migracao-shaders-r2.sql
ALTER TABLE shaders ADD COLUMN hash TEXT;
CREATE TABLE IF NOT EXISTS shader_r2 (
  hash TEXT PRIMARY KEY,       -- sha256 do conteúdo original
  jogo TEXT NOT NULL,
  bytes INTEGER NOT NULL,      -- tamanho comprimido no R2
  quando INTEGER NOT NULL
);
-- contadores pequenos (uma linha cada): 'shaders_r2' = bytes ocupados no R2 pelos shaders (teto em SHADERS_TETO)
CREATE TABLE IF NOT EXISTS contadores (
  nome TEXT PRIMARY KEY,
  valor INTEGER NOT NULL
);
