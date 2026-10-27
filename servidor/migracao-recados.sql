-- 27/10/2026: the owner's notice to every launcher, shown once as a pop-up. Once, on a database made before:
--   npx wrangler d1 execute otavix-launcher --remote --file=migracao-recados.sql
CREATE TABLE IF NOT EXISTS recados (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  texto TEXT NOT NULL,
  criado INTEGER NOT NULL,
  ativo INTEGER NOT NULL DEFAULT 1
);
