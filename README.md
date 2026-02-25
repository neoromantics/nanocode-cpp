# nanocode-cpp

A C++23 rewrite of nanocode, a minimal agentic coding assistant. This version natively implements tool usage and API interactions without Python dependencies, using Boost Asio for asynchronous operations.

## Features

- Complete native C++ implementation
- Interactive agentic loop with `linenoise` (up-arrow history support)
- Server-Sent Events (SSE) streaming for real-time text output
- Built-in tools: `read`, `write`, `edit`, `glob`, `grep`, `bash`, `fetch_url`, `execute_python`
- Conversational persistence (`/save` and `/load`)
- API support for Gemini, Anthropic, and OpenRouter
- Configuration via `.nanocoderc` and CLI arguments
- Asynchronous networking using Boost coroutines

## Prerequisites

- CMake 3.20 or newer
- C++23 compatible compiler (e.g., Apple Clang 15+)
- Boost libraries (Components: json)
- OpenSSL

## Build Instructions

1. Create a build directory:
   ```bash
   mkdir build
   cd build
   ```

2. Run CMake and build:
   ```bash
   cmake ..
   make
   ```

## Usage

Set one or more of the following environment variables:
- `GEMINI_API_KEY`
- `ANTHROPIC_API_KEY`
- `OPENROUTER_API_KEY`

Optionally, set the `MODEL` environment variable to override defaults.

Run the executable:
```bash
./build/nanocode
```

### Commands
- `/model <model_name>` - Switch the active model seamlessly mid-conversation.
- `/save <file.json>` - Save the current conversation history to a JSON file.
- `/load <file.json>` - Load a previously saved conversation history.
- `/c` - Clear the context and message history.
- `/q` or `exit` - Quit the application.

## License

MIT
