# cardputer (Claude Code channel plugin)

Lets an M5Stack Cardputer ADV send messages into an interactive Claude Code
session and shows the replies on the device. Same mechanism as the official
Telegram/Discord channel plugins (an MCP server with the `claude/channel`
capability), but the transport is a tiny HTTP API you expose with
`tailscale funnel` or `cloudflared`.

```
Cardputer ADV ──HTTPS──▶ funnel/tunnel ──▶ 127.0.0.1:8790 (server.mjs) ──stdio MCP──▶ claude
                                                                          ◀── reply tool
```

## Install (any machine: Omarchy, macOS…)

Needs Node ≥ 18 on PATH (no npm packages).

```
claude plugin marketplace add tern-yu/cardputer-claude    # or a local path to this repo
claude plugin install cardputer@tern-mods
```

Put `plugins/cardputer/bin/claude-cardputer` on your PATH (symlink is fine), then:

```
claude-cardputer            # = claude --dangerously-load-development-channels plugin:cardputer@tern-mods
```

Claude Code asks once to confirm loading a development channel. On first run the
server creates `~/.claude/channels/cardputer/token` and prints
`cardputer: http://127.0.0.1:8790`.

## Expose

```
tailscale funnel --bg 8790          # https://<host>.<tailnet>.ts.net
```

Test from a phone browser: `https://<host>.<tailnet>.ts.net/?token=<token>`.

## API (bearer token)

| Method | Path | Body / query | Returns |
| --- | --- | --- | --- |
| GET | `/api/ping` | | `{ok, host, cwd, seq, pending}` |
| POST | `/api/send` | `text/plain` body or `{text}` | `{ok, message_id}` |
| GET | `/api/poll` | `after=<seq>&wait=<0-25>` | `{events, seq}` (long-poll) |
| POST | `/api/perm` | `{request_id \| "latest", behavior: allow\|deny}` | `{ok}` |

Event types: `reply` `{id,text}`, `edit` `{id,text}`, `perm`
`{request_id,tool_name,input_preview}`, `perm_done`, `sent` (echo of your own message).

Env: `CARDPUTER_PORT` (8790), `CARDPUTER_HOST` (127.0.0.1), `CARDPUTER_STATE_DIR`,
`CARDPUTER_TOKEN` (override the token file).

## Security notes

- Bind stays on loopback; only the tunnel reaches it. The token is the whole gate —
  treat it like a password, keep it on the SD card.
- One session owns the port. A second `claude-cardputer` just logs "port in use";
  set `CARDPUTER_PORT` to run two.
- The device may answer permission prompts (the server declares
  `claude/channel/permission`), so anyone holding the token can approve tool calls.
