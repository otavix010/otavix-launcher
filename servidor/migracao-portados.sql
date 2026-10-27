-- 26/10/2026: a requested game the owner marked as ported (when; 0 = not yet). Once, on a database made before:
--   npx wrangler d1 execute otavix-launcher --remote --file=migracao-portados.sql
ALTER TABLE jogos ADD COLUMN portado INTEGER NOT NULL DEFAULT 0;
