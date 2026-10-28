-- 28/10: /v1/respostas procura as respostas da instalação; sem índice varria a tabela inteira a cada launcher aberto
CREATE INDEX IF NOT EXISTS reports_install ON reports (install);
