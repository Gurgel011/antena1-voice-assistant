# Antena 1 — assistente de voz local

Este repositório é uma customização do firmware open source
[xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) para o assistente de voz
local da Antena 1 (placa ESP-VoCat), mais um servidor de voz em Python que roda
na rede local em vez de depender do serviço em nuvem `xiaozhi.me` usado pelo
projeto original.

Para a documentação do framework em si (protocolo MCP, protocolo WebSocket,
como criar uma placa customizada, estilo de código), veja
[`docs/`](docs/) e o [`README.md`](README.md) na raiz — este documento cobre
apenas a camada específica da Antena 1.

## O que este repositório contém

- Firmware ESP-IDF para a placa ESP-VoCat, com a palavra de ativação original
  em chinês desativada, tela e logotipo customizados da Antena 1, e apontando
  para o servidor de voz local em vez do serviço em nuvem.
- Dois servidores Python em [`local_voice_assistant/`](local_voice_assistant/):
  um servidor HTTP (`server.py`) e um servidor WebSocket (`xiaozhi_ws_server.py`)
  — ver [Os dois servidores](#os-dois-servidores) abaixo.
- Assets de exibição e o player de stream de rádio da Antena 1.

Artefatos de execução **intencionalmente não versionados**: `build/`,
`managed_components/`, ambientes virtuais Python, e os modelos de STT/TTS em
`local_voice_assistant/models/` (grandes demais para versionar — ver
[`local_voice_assistant/models/README.md`](local_voice_assistant/models/README.md)).

## Os dois servidores

O firmware conversa com o servidor WebSocket. O servidor HTTP existe à parte,
para testes e integrações que não falam o protocolo do dispositivo.

| | `server.py` | `xiaozhi_ws_server.py` |
|---|---|---|
| Protocolo | HTTP/REST | WebSocket (protocolo do dispositivo xiaozhi, ver [`docs/websocket.md`](docs/websocket.md)) |
| Porta padrão | 8088 | 8089 |
| Usado pelo firmware? | Não diretamente | **Sim** — é este que o aparelho físico usa |
| Para que serve | Testar por `curl`, integrações externas | Conversa real com o aparelho: streaming de áudio, controle de rádio/volume, detecção de fala por voz |

`xiaozhi_ws_server.py` importa `server.py` como biblioteca e reaproveita suas
funções de transcrição (Whisper/Vosk), síntese (Piper) e detecção de comando —
não é um servidor concorrente, é uma camada em cima. **Os dois processos rodam
ao mesmo tempo**, em portas diferentes; nenhum inicia o outro.

## Compilação do firmware

```bash
. <caminho-do-esp-idf>/export.sh
idf.py set-target esp32s3
idf.py build
idf.py -p <porta-serial> flash
```

Use ESP-IDF 5.5.2 ou mais recente, compatível com o manifesto do projeto.

A configuração padrão usa a placa ESP-VoCat, desativa a palavra de ativação
original em chinês, usa a tela customizada da Antena 1, e aponta o cliente de
voz local e o OTA para um endereço de exemplo — **isso precisa ser trocado
antes de compilar para uso real**:

1. **`sdkconfig.defaults`** — a linha `CONFIG_OTA_URL` traz um IP de exemplo
   (faixa reservada para documentação, RFC 5737). Troque pelo IP real do
   servidor, ou sobreponha localmente via `idf.py menuconfig` (o arquivo
   `sdkconfig` gerado não é versionado).
2. **`main/application.cc`** — as constantes `kLocalVoiceServerHost` e
   `kLocalVoiceServerPort` (próximo ao topo do arquivo) apontam para o mesmo
   servidor local e **estão fixas no código-fonte**, não numa opção de
   configuração. Edite-as diretamente antes de compilar, com o IP e a porta do
   servidor (`server.py`, porta 8088 por padrão) na sua rede.

Nenhuma senha de Wi-Fi fica no código: o provisionamento é feito em tempo de
execução (hotspot ou BluFi — ver [`docs/blufi.md`](docs/blufi.md)), não
compilado no firmware.

## Servidor de voz local

### Instalação

```bash
cd local_voice_assistant
python3 -m venv .venv
. .venv/bin/activate
pip install -r requirements.txt
```

Requisitos adicionais fora do `requirements.txt`:

- `ffmpeg` instalado no sistema (conversão de áudio nos dois servidores).
- Ollama em execução localmente (`127.0.0.1:11434`), com o modelo `qwen2.5:3b`
  disponível (`ollama pull qwen2.5:3b`).
- Executável do Piper disponível como `piper` no `PATH` ou em
  `~/.local/bin/piper`.
- Modelos locais nos caminhos descritos em
  [`local_voice_assistant/models/README.md`](local_voice_assistant/models/README.md).

Configuração (opcional — todas as variáveis têm valor padrão razoável):

```bash
cp local_voice_assistant/.env.example local_voice_assistant/.env
# os scripts nao leem .env automaticamente; exporte antes de iniciar:
set -a; . local_voice_assistant/.env; set +a
```

### Executar

Os dois servidores rodam como processos separados, ao mesmo tempo:

```bash
# Servidor HTTP (porta 8088) — testes e integrações externas
python3 local_voice_assistant/server.py

# Servidor WebSocket (porta 8089) — o que o aparelho realmente usa
python3 local_voice_assistant/xiaozhi_ws_server.py
```

### Verificação

```bash
# Servidor HTTP
curl http://127.0.0.1:8088/health
# esperado: {"ok": true, "model": "qwen2.5:3b"}

# Servidor WebSocket (checagem HTTP simples, sem abrir uma sessão de voz)
curl http://127.0.0.1:8089/time
```

### Endpoints do servidor HTTP (`server.py`, porta 8088)

Corpo de áudio: bytes crus (WAV/MP3/OGG, detectado pelo `Content-Type`).

| Rota | Método | Entrada | Saída |
|---|---|---|---|
| `/health` | GET | — | `{"ok": true, "model": "..."}` |
| `/transcribe` | POST | áudio | `{"ok": true, "text": "..."}` |
| `/voice` | POST | áudio | `{"ok": true, "text", "command", "answer", "route"}` |
| `/voice-ogg` | POST | áudio | áudio `audio/ogg` + cabeçalhos `X-Command`, `X-Transcript`, `X-Route`, `X-Answer` |
| `/chat` | POST | JSON `{"text": "..."}` | JSON igual a `/voice` |
| `/tts` | POST | JSON `{"text": "..."}` | áudio `audio/ogg` |

### Protocolo do servidor WebSocket (`xiaozhi_ws_server.py`, porta 8089)

Implementa o handshake e as mensagens descritas em
[`docs/websocket.md`](docs/websocket.md) (`hello`, `listen`, `stt`, `tts`,
`mcp`), com áudio Opus mono, 16 kHz na entrada e 24 kHz na saída. Também expõe
`GET /time` (texto simples, para diagnóstico rápido de conectividade).

Comandos de voz reconhecidos durante uma sessão: iniciar/parar a rádio Antena 1
e aumentar/diminuir volume — ver as constantes no topo do arquivo para os
limiares de detecção de fala e os valores de volume.

## Diagnóstico

```bash
# O Ollama está de pé e com o modelo certo?
curl -s http://127.0.0.1:11434/api/tags

# Teste de texto, sem áudio (isola o LLM/roteamento do reconhecimento de fala)
curl -s -X POST http://127.0.0.1:8088/chat \
  -H "Content-Type: application/json" \
  -d '{"text": "que horas sao"}'

# Teste de síntese
curl -s -X POST http://127.0.0.1:8088/tts \
  -H "Content-Type: application/json" \
  -d '{"text": "teste"}' -o /tmp/teste.ogg
file /tmp/teste.ogg
```

Arquivos de depuração de áudio (último áudio recebido/convertido) ficam em
`/tmp/local_voice_last_input.*` e `/tmp/local_voice_last_converted.wav` quando
o servidor HTTP processa uma requisição de voz.
