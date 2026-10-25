# Servidor do Otavix Launcher

Recebe os **reports de problema**, os **pedidos de port** (com os rankings da semana, do mês e de todos os tempos) e o **desempenho** das partidas de quem aceitou compartilhar. Roda de graça na Cloudflare (Workers + banco D1).

**No ar desde 25/10/2026:** `https://otavix-launcher.otavix-launcher-servidor.workers.dev`, banco `otavix-launcher` na conta do Otavix. Os reports ficam só guardados no banco e são lidos na aba **Administração** do launcher, com a chave de administrador; por escolha do dono, não há e-mail (o Resend, abaixo, é opcional e não está configurado).

Para montar de novo, ou em outra conta:

1. Crie uma conta grátis em https://dash.cloudflare.com. Só se quiser os reports também por e-mail: uma conta em https://resend.com e uma API key em "API Keys".
2. Num terminal nesta pasta:
   ```
   npx wrangler login
   npx wrangler d1 create otavix-launcher
   ```
   Copie o `database_id` que ele mostrar para o `wrangler.toml`.
3. Crie as tabelas e as senhas:
   ```
   npx wrangler d1 execute otavix-launcher --remote --file=schema.sql
   (banco criado antes de 25/10/2026: rode uma vez também o migracao-respostas.sql, que acrescenta a resposta do dono aos reports)
   npx wrangler secret put ADMIN_TOKEN      (uma senha longa, só sua)
   npx wrangler secret put RESEND_API_KEY   (opcional: a chave do Resend)
   npx wrangler secret put REPORT_TO        (opcional: o e-mail da conta do Resend)
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
