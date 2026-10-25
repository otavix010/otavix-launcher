-- 25/10/2026: the owner's answer to a report, read by the launcher that sent it. Once, on a database made before:
--   npx wrangler d1 execute otavix-launcher --remote --file=migracao-respostas.sql
ALTER TABLE reports ADD COLUMN resposta TEXT NOT NULL DEFAULT '';
ALTER TABLE reports ADD COLUMN respondido INTEGER NOT NULL DEFAULT 0;
