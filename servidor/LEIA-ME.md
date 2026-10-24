# Servidor do Otavix Launcher

Recebe os **reports de problema**, que ficam guardados e chegam no seu e-mail, e os **pedidos de port**, com os rankings da semana, do mês e de todos os tempos. Roda de graça na Cloudflare (Workers + banco D1). O e-mail sai pelo Resend, que no plano grátis manda 3.000 por mês para o próprio dono da conta, sem precisar de domínio.

Nada disso está no ar ainda. Para ativar:

1. Crie uma conta grátis em https://dash.cloudflare.com e outra em https://resend.com. No Resend, gere uma API key em "API Keys".
2. Num terminal nesta pasta:
   ```
   npx wrangler login
   npx wrangler d1 create otavix-launcher
   ```
   Copie o `database_id` que ele mostrar para o `wrangler.toml`.
3. Crie as tabelas e as senhas:
   ```
   npx wrangler d1 execute otavix-launcher --remote --file=schema.sql
   npx wrangler secret put ADMIN_TOKEN      (uma senha longa, só sua)
   npx wrangler secret put RESEND_API_KEY   (a chave do Resend)
   npx wrangler secret put REPORT_TO        (o e-mail da conta do Resend)
   npx wrangler deploy
   ```
4. O deploy mostra o endereço, algo como `https://otavix-launcher.<sua-conta>.workers.dev`. Coloque-o em `hub.toml`, em `[servidor] url`.
5. No seu launcher, em Configurações do launcher → Sobre → Chave de administrador, cole o mesmo `ADMIN_TOKEN`. A aba "Administração" aparece só para você.

## Teste local, sem conta

```
npx wrangler d1 execute otavix-launcher --local --file=schema.sql
npx wrangler dev --local --port 8787 --var ADMIN_TOKEN:teste
```

Depois aponte o `hub.toml` para `http://127.0.0.1:8787`. Sem as chaves do Resend, os reports só ficam guardados.
