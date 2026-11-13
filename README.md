# Coarena C Client

A terminal-based Coarena API client written in C11. It uses libcurl for HTTPS
requests and json-c for JSON handling. The code is divided into small modules
so that API communication, attachments, output files, rendering, and session
storage can be maintained separately.

## Features

- SSE-based chat completions using Coarena's OpenAI-compatible endpoint; the completed response is formatted after reception.
- Model discovery and switching with `/models` and `/model`.
- Usage display with `/usg` (remaining USD and UTC reset time).
- Multiple API keys in `key.txt`, one key per line.
- Multi-key selection: with more than one key, the client checks balances in
  file order and prefers the first key with **more than $0.20** remaining. If
  none qualifies, it chooses the first key with a positive balance. With one
  key, balance-based selection is skipped.
- Local image and text-file prompts using `/image` and `/file`.
- Markdown-oriented terminal rendering, including bold, italic, headings,
  lists, blockquotes, inline code, and fenced code blocks.
- Model-generated files marked with `file:filename.ext` fenced blocks are
  written into `CoarenaFiles/` with unique names.
- Persistent local chat sessions in `CoarenaSessions/`.

## Requirements

The project targets Debian-based distributions such as antiX Linux.

- C11 compiler (GCC or Clang)
- CMake 3.16 or newer
- pkg-config
- libcurl development files
- json-c development files

On antiX/Debian, install the dependencies with:

```sh
./install.sh
```

The equivalent package list is:

```sh
sudo apt install build-essential cmake pkg-config \
    libcurl4-openssl-dev libjson-c-dev
```

## Build

Run:

```sh
./make.sh
```

This configures the project with CMake and builds the executable at
`build/coarena`.

To use CMake directly:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

To install the compiled executable system-wide:

```sh
cmake --install build
```

## API key setup

Create `key.txt` in the same directory as the executable. The file supports
one key per line; blank lines and lines beginning with `#` are ignored.

```text
coa_FIRST_API_KEY
coa_SECOND_API_KEY
coa_THIRD_API_KEY
```

Restrict access to the file:

```sh
chmod 600 key.txt
```

A template is provided as `key.txt.example`. After building, copy it beside
the executable (for the default build, that is `build/key.txt`) and replace
the placeholders:

```sh
cp key.txt.example build/key.txt
chmod 600 build/key.txt
$EDITOR build/key.txt
```

**Never commit `key.txt` or share it publicly.** Any key previously posted in
a chat, issue, log, or repository should be revoked and replaced.

The multi-key selector does not divide a single request across keys. A key can
have a positive balance that is still insufficient for a particular request;
Coarena may reject that request. This client currently does not automatically
retry a failed completion using another key.

## Run

From the directory containing the executable and `key.txt`:

```sh
./build/coarena
```

The client creates `CoarenaFiles/` and `CoarenaSessions/` beside the running
executable when possible. It also accepts `key.txt` from the current working
directory as a fallback when no adjacent key file can be opened.

## Commands

| Command | Description |
|---|---|
| `/help` | Show command help |
| `/usg` | Show the selected key's remaining USD and UTC reset time |
| `/models` | Fetch and list currently offered models |
| `/model` | List models and show the active model |
| `/model <id-or-number>` | Switch the active model |
| `/new` | Save the current conversation and start a new session |
| `/history` | List saved local sessions |
| `/open <number-or-id>` | Reopen a saved session |
| `/reset` | Clear the current conversation |
| `/image "path" \| prompt` | Send an image with a prompt |
| `/file "path" \| prompt` | Attach a text file to a prompt |
| `/exit`, `/quit` | Save and exit |

Examples:

```text
/image "/home/user/diagram.png" | Explain the architecture.
/file "/home/user/server.c" | Review this source for memory errors.
/model 2
```

Supported image extensions are JPG, JPEG, PNG, GIF, and WEBP. Images are
limited to 8 MiB. Text attachments are limited to 2 MiB and reject binary
content. Returned files have a 25 MiB per-file safety limit.

## Project layout

```text
coarena-client/
├── CMakeLists.txt
├── README.md
├── .gitignore
├── install.sh
├── make.sh
├── key.txt.example
├── include/
│   └── coarena.h
└── src/
    ├── main.c         # Interactive command loop
    ├── core.c         # Paths, buffers, and common utilities
    ├── keys.c         # API key loading and memory cleanup
    ├── attachments.c  # Image/text message construction
    ├── api.c          # HTTP, streaming, model and usage APIs
    ├── files.c        # Returned-file extraction and safe writes
    ├── render.c       # Terminal Markdown rendering
    └── sessions.c     # Local session persistence and history
```

## Data and privacy

- `key.txt` contains credentials and should remain private.
- Conversation sessions are stored locally as JSON under `CoarenaSessions/`.
- Generated files are stored under `CoarenaFiles/`.
- Prompts and attachments are transmitted to Coarena and the selected model
  provider. Do not submit passwords, access tokens, or confidential material
  unless you have reviewed the providers' current data-handling terms.

## API endpoints used

The client currently uses these Coarena API paths:

- `POST /api/v1/chat/completions` — streamed chat completion.
- `GET /api/v1/limits` — daily usage and quota information.
- `GET /api/v1/models` — model catalogue.

The API response format and these endpoints are controlled by Coarena and may
change. Check Coarena's current API documentation if a request starts failing.

## Limitations

- The `/file` command sends text-file contents as text; it is not a general
  binary upload mechanism.
- A generated file is saved only when its content is present in the model's
  response (for example, in a `file:filename.ext` fenced block) or in a
  recognized structured attachment object. The client cannot save a file that
  the API did not return.
- Session history is local to this executable's data directory; it is not
  synchronized with Coarena's web interface.
- Live requests require working network access and a valid Coarena API key.

## License

No license has been assigned yet. Add a license file before redistributing the
project under an open-source license.
